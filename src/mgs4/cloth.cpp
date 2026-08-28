#include "mgs4/cloth.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"

// Cloth timing.
//
// Earlier attempts here gated the cloth producer on the engine's 60 Hz tick and
// nothing else. That fixed the sway rate but consistently produced a doubled,
// semi-transparent cloth, through several variations of what the skipped frame
// did. The reason is structural rather than a detail of the skip: cloth is not
// timed in isolation.
//
// The engine funnels simulation timing through one function, called here the
// task timing function. It reads the real frame delta, multiplies by 59.94 to
// express the frame in 60 Hz ticks, and splits that into a whole substep count
// plus a remainder, handing the caller both a step size and a step count. Every
// simulation task asks it how much time this frame is worth.
//
// Gating the cloth producer while leaving that function alone puts cloth and
// everything feeding it on different clocks: the producer runs at 60 Hz while
// the rest of the pipeline advances at the full frame rate. Two parts of the
// same garment then disagree about what time it is, which is what showed up on
// screen as two overlapping layers.
//
// So the fix is a set rather than a single hook:
//
//   * The task timing function substitutes the exact frame delta for a task
//     whose stock step would over-advance it, so ordinary simulation tasks
//     advance by real time.
//   * Cloth is deliberately excluded from that substitution and gated instead,
//     keeping its native step. Cloth solvers are stiff and are tuned for a
//     fixed step, so feeding them a shorter one changes how the cloth behaves
//     rather than just how fast it moves.
//   * Which of the two applies is decided by a per-thread flag set around the
//     cloth entry points, because the timing function is shared and the cloth
//     work runs on job threads.
//
// The flags have to be per-thread, not global: this timing function is called
// from the engine's job threads, so a global would let one thread's cloth work
// change the timing another thread sees.

namespace
{
    constexpr const char* kSpursTaskTimingSignature =
        "48 89 5C 24 08 57 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 0B 05 ?? ?? ?? ?? "
        "0F 29 74 24 30 0F 29 7C 24 20 8B FA";

    constexpr const char* kClothManagerUpdateSignature =
        "48 89 74 24 10 57 48 83 EC 30 48 8B 81 80 03 00 00 41 8B F0 0F 29 74 24 20 "
        "48 8B F9 0F 28 F1 83 38 00";

    constexpr const char* kClothProducerUpdateSignature =
        "48 89 5C 24 18 55 57 41 56 48 81 EC 10 01 00 00 48 8B B9 70 03 00 00 "
        "45 33 F6 41 8B E8 48 8B D9";

    // The transform publish entry point, called on frames where the solver is
    // skipped so the renderer still gets a transform rather than nothing.
    constexpr const char* kClothTransformPublishSignature =
        "48 8B 41 08 4C 8B D1 83 38 00 C7 40 04 00 00 00 00 74 ?? 49 8B 42 08 "
        "4C 63 40 04 44 3B 00";

    // A step longer than this is not a plausible per-frame simulation step and
    // is left alone: it means the caller is doing something other than
    // advancing one frame.
    constexpr float kMaximumReasonableStep = 0.1f;

    using SpursTaskTimingFn = uint64_t(__fastcall*)(float*, uint32_t);
    using ClothManagerUpdateFn = void(__fastcall*)(uint8_t*, float, int32_t);
    using ClothProducerUpdateFn = void(__fastcall*)(uint8_t*, float, int32_t);
    using ClothTransformPublishFn = void(__fastcall*)(uint8_t*);

    SafetyHookInline g_spursTaskTiming{};
    SafetyHookInline g_clothManagerUpdate{};
    SafetyHookInline g_clothProducerUpdate{};
    ClothTransformPublishFn g_clothTransformPublish = nullptr;

    const float* g_frameDeltaSeconds = nullptr;
    const int32_t* g_frameTickDelta60 = nullptr;

    // Per-thread, deliberately: the timing function is shared across the
    // engine's job threads.
    thread_local bool t_inClothManager = false;
    thread_local bool t_inClothProducer = false;

    std::atomic<uint64_t> g_producerCalls{0};
    std::atomic<uint64_t> g_producerGated{0};
    std::atomic<uint64_t> g_stepsSubstituted{0};

    bool IsNativeTick()
    {
        return g_frameTickDelta60 == nullptr || *g_frameTickDelta60 != 0;
    }

    uint64_t __fastcall SpursTaskTimingHook(float* taskStep, uint32_t maxSteps)
    {
        uint64_t stepCount = g_spursTaskTiming.unsafe_call<uint64_t>(taskStep, maxSteps);
        if (!taskStep)
            return stepCount;

        // In gate mode cloth keeps its native step and is rate-limited by
        // gating instead, so it is excluded here. In delta mode it is treated
        // like any other task and falls through to the substitution below.
        if ((t_inClothManager || t_inClothProducer) &&
            config::Get().clothMode == config::Settings::ClothMode::Gate)
            return stepCount;

        const float exactDelta = g_frameDeltaSeconds ? *g_frameDeltaSeconds : 0.0f;
        const float stockStep = *taskStep;

        const bool overAdvancing = exactDelta > 0.0f && std::isfinite(exactDelta) &&
                                   std::isfinite(stockStep) && stockStep > exactDelta &&
                                   stockStep <= kMaximumReasonableStep;
        if (!overAdvancing)
            return stepCount;

        // One step of exactly this frame's duration: the task advances by real
        // time rather than by a step sized for 60 Hz.
        *taskStep = exactDelta;
        g_stepsSubstituted.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    void __fastcall ClothManagerUpdateHook(uint8_t* manager, float updateArgument,
                                           int32_t updateType)
    {
        const bool previous = t_inClothManager;
        t_inClothManager = true;
        g_clothManagerUpdate.unsafe_call(manager, updateArgument, updateType);
        t_inClothManager = previous;
    }

    void __fastcall ClothProducerUpdateHook(uint8_t* producer, float updateArgument,
                                            int32_t updateType)
    {
        g_producerCalls.fetch_add(1, std::memory_order_relaxed);

        // Delta mode never skips: the solver runs every frame and the shared
        // task timing hook feeds it the real frame delta.
        if (config::Get().clothMode == config::Settings::ClothMode::Delta)
        {
            const bool previous = t_inClothProducer;
            t_inClothProducer = true;
            g_clothProducerUpdate.unsafe_call(producer, updateArgument, updateType);
            t_inClothProducer = previous;
            return;
        }

        if (IsNativeTick())
        {
            const bool previous = t_inClothProducer;
            t_inClothProducer = true;
            g_clothProducerUpdate.unsafe_call(producer, updateArgument, updateType);
            t_inClothProducer = previous;
            return;
        }

        // Skipped frame: publish the transform the last simulated frame produced
        // so the renderer draws something current-looking, without advancing the
        // solver.
        g_producerGated.fetch_add(1, std::memory_order_relaxed);
        if (config::Get().clothPublishOnSkip && producer && g_clothTransformPublish)
            g_clothTransformPublish(producer);
    }
} // namespace

bool mgs4::InstallClothTiming(const void* frameTimingStruct, const void* frameTickDelta60)
{
    if (!frameTimingStruct || !frameTickDelta60)
    {
        logging::Error("cloth: the frame timing globals are unavailable");
        return false;
    }

    // Frame delta sits a fixed distance into the same struct the cutscene fix
    // already resolves, so it does not need a separate signature.
    constexpr ptrdiff_t kFrameDeltaOffset = 0x18;
    g_frameDeltaSeconds = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(frameTimingStruct) + kFrameDeltaOffset);
    g_frameTickDelta60 = static_cast<const int32_t*>(frameTickDelta60);
    logging::Address("frameDeltaSeconds", reinterpret_cast<uintptr_t>(g_frameDeltaSeconds));

    // Sanity check the value before trusting it: a plausible frame delta is
    // between 1000 fps and 10 fps.
    const float delta = *g_frameDeltaSeconds;
    if (!(delta > 0.0005f && delta < 0.2f) || !std::isfinite(delta))
        logging::Warn("cloth: frame delta reads {}, which looks wrong", delta);

    const module_info::Section& text = module_info::Text();

    struct Target
    {
        const char* name;
        const char* signature;
        uint8_t* address;
    };

    Target targets[] = {
        {"spursTaskTiming", kSpursTaskTimingSignature, nullptr},
        {"clothManagerUpdate", kClothManagerUpdateSignature, nullptr},
        {"clothProducerUpdate", kClothProducerUpdateSignature, nullptr},
        {"clothTransformPublish", kClothTransformPublishSignature, nullptr},
    };

    for (Target& target : targets)
    {
        const size_t matches = memory::CountMatches(text, target.signature);
        if (matches != 1)
        {
            logging::Error("cloth: {} matched {} times, expected 1", target.name, matches);
            return false;
        }
        target.address = memory::Scan(text, target.signature);
        logging::Address(target.name, reinterpret_cast<uintptr_t>(target.address));
    }

    g_clothTransformPublish = reinterpret_cast<ClothTransformPublishFn>(targets[3].address);

    g_spursTaskTiming =
        safetyhook::create_inline(targets[0].address, reinterpret_cast<void*>(&SpursTaskTimingHook));
    g_clothManagerUpdate = safetyhook::create_inline(
        targets[1].address, reinterpret_cast<void*>(&ClothManagerUpdateHook));
    g_clothProducerUpdate = safetyhook::create_inline(
        targets[2].address, reinterpret_cast<void*>(&ClothProducerUpdateHook));

    if (!g_spursTaskTiming || !g_clothManagerUpdate || !g_clothProducerUpdate)
    {
        logging::Error("cloth: one or more hooks failed to install");
        g_spursTaskTiming = {};
        g_clothManagerUpdate = {};
        g_clothProducerUpdate = {};
        g_clothTransformPublish = nullptr;
        return false;
    }

    logging::Info("cloth: task timing and cloth gating installed");
    return true;
}

void mgs4::LogClothCounters()
{
    const uint64_t calls = g_producerCalls.load(std::memory_order_relaxed);
    if (calls == 0)
        return;

    const uint64_t gated = g_producerGated.load(std::memory_order_relaxed);
    logging::Info("cloth: producer called {} times, {} gated ({:.1f}%), {} task steps substituted",
                  calls, gated, 100.0 * static_cast<double>(gated) / static_cast<double>(calls),
                  g_stepsSubstituted.load(std::memory_order_relaxed));
}
