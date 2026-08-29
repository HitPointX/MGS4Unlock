#include "mgs4/cloth.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"
#include "mgs4/timing.h"

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
//     whose stock step would over-advance it, so simulation advances by real
//     time rather than by a step sized for 60 Hz.
//   * Cloth goes through that same substitution and is simulated every frame.
//     The obvious alternative -- keep cloth's native fixed step and run the
//     solver only on 60 Hz frames -- was tried first and at length. It fixes
//     the rate, but every variation of it left a doubled, semi-transparent
//     copy of the garment on screen, because a garment whose solver only runs
//     on some frames ends up inconsistent with the parts of the pipeline that
//     run on all of them. Simulating every frame has none of that. The stiff
//     solver copes with the shorter step better than the mismatch.
//   * A per-thread flag around the cloth entry points still marks cloth work,
//     so the gated mode remains available for comparison. It is per-thread
//     because the timing function is shared and cloth runs on job threads.
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

    // Snake's bandana runs through a separate hair solver, and it does not want
    // the real frame delta the way the rest of the simulation does. Fed a much
    // shorter step it floats up off his head instead of draping: gravity is
    // integrated per step and so scales down with the step, while the chain
    // constraints keep their stiffness, so the chain never settles.
    //
    // Giving it a fixed step near 1/60 regardless of framerate keeps the solve
    // in the regime it was tuned for. The step is the engine's own reference
    // value rather than a round 1/60.
    constexpr const char* kHairSimulationUpdateSignature =
        "40 53 48 81 EC D0 00 00 00 48 8B D9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
        "48 8B 81 48 02 00 00 48 85 C0";

    constexpr float kHairReferenceStep = 0.016683351f; // 1/59.94
    constexpr ptrdiff_t kHairChainCountOffset = 0x260;

    // A step longer than this is not a plausible per-frame simulation step and
    // is left alone: it means the caller is doing something other than
    // advancing one frame.
    constexpr float kMaximumReasonableStep = 0.1f;

    using SpursTaskTimingFn = uint64_t(__fastcall*)(float*, uint32_t);
    using ClothManagerUpdateFn = void(__fastcall*)(uint8_t*, float, int32_t);
    using ClothProducerUpdateFn = void(__fastcall*)(uint8_t*, float, int32_t);
    using ClothTransformPublishFn = void(__fastcall*)(uint8_t*);

    SafetyHookInline g_spursTaskTiming{};
    SafetyHookInline g_hairSimulationUpdate{};
    SafetyHookInline g_clothManagerUpdate{};
    SafetyHookInline g_clothProducerUpdate{};
    ClothTransformPublishFn g_clothTransformPublish = nullptr;

    const float* g_frameDeltaSeconds = nullptr;
    const int32_t* g_frameTickDelta60 = nullptr;

    // Per-thread, deliberately: the timing function is shared across the
    // engine's job threads.
    thread_local bool t_inClothManager = false;
    thread_local bool t_inClothProducer = false;
    thread_local bool t_inHair = false;
    thread_local bool t_inJacket = false;

    std::atomic<uint64_t> g_producerCalls{0};
    std::atomic<uint64_t> g_producerGated{0};
    std::atomic<uint64_t> g_stepsSubstituted{0};
    std::atomic<uint64_t> g_hairCalls{0};
    std::atomic<uint32_t> g_seenChainCounts[8]{};
    std::atomic<bool> g_deltaChecked{false};

    // Checked on first use rather than at install time. At install the game has
    // not rendered a frame yet, so the delta is legitimately zero and warning
    // about it only produces a misleading line in every user's log.
    void CheckFrameDeltaOnce(float delta)
    {
        if (delta == 0.0f || g_deltaChecked.load(std::memory_order_relaxed))
            return;

        bool expected = false;
        if (!g_deltaChecked.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            return;

        // Anything from 1000 fps down to 10 fps is plausible.
        if (delta > 0.0005f && delta < 0.2f && std::isfinite(delta))
            logging::Info("cloth: frame delta is {:.5f}s ({:.0f} fps)", delta, 1.0f / delta);
        else
            logging::Warn("cloth: frame delta reads {}, which is outside the plausible range",
                          delta);
    }

    bool IsNativeTick()
    {
        return g_frameTickDelta60 == nullptr || *g_frameTickDelta60 != 0;
    }

    uint64_t __fastcall SpursTaskTimingHook(float* taskStep, uint32_t maxSteps)
    {
        uint64_t stepCount = g_spursTaskTiming.unsafe_call<uint64_t>(taskStep, maxSteps);
        if (!taskStep)
            return stepCount;

        // The hair solver is unstable on a short step, so it gets a fixed one
        // near 1/60 no matter the framerate. Returning a single step stops the
        // engine compensating by running several.
        if (t_inHair && config::Get().hairFixedStep)
        {
            *taskStep = kHairReferenceStep;
            return maxSteps < 1 ? maxSteps : 1;
        }

        // In gate mode cloth keeps its native step and is rate-limited by
        // gating instead, so it is excluded here. In delta mode it is treated
        // like any other task and falls through to the substitution below.
        if ((t_inClothManager || t_inClothProducer) &&
            config::Get().clothMode == config::Settings::ClothMode::Gate)
            return stepCount;

        // The jacket solver already receives a correct per-frame delta from its
        // caller, confirmed by logging both and finding them identical.
        // Substituting a step and forcing a single substep therefore changes how
        // it integrates without fixing anything, so leave its stepping alone.
        if (t_inJacket && config::Get().excludeJacketFromTaskTiming)
            return stepCount;

        const float exactDelta = g_frameDeltaSeconds ? *g_frameDeltaSeconds : 0.0f;
        const float stockStep = *taskStep;

        CheckFrameDeltaOnce(exactDelta);

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

    // Records a chain count if it has not been seen before, so the log shows
    // every distinct hair instance in play rather than only the most recent.
    void RecordChainCount(uint32_t value)
    {
        if (value == 0)
            return;

        for (auto& slot : g_seenChainCounts)
        {
            uint32_t existing = slot.load(std::memory_order_relaxed);
            if (existing == value)
                return;
            if (existing == 0 &&
                slot.compare_exchange_strong(existing, value, std::memory_order_relaxed))
                return;
        }
    }

    std::atomic<uint64_t> g_hairGated{0};

    void __fastcall HairSimulationUpdateHook(uint8_t* hair)
    {
        g_hairCalls.fetch_add(1, std::memory_order_relaxed);

        // Hair hangs off animated bones exactly as cloth does, so during a
        // cutscene it has to advance at the rate the animation moves them.
        // Otherwise it takes several steps against a motionless head and then
        // lurches, which reads as exaggerated, floaty hair.
        if (config::Get().clothFollowsCutscene && mgs4::IsCutsceneAdvancing() && !IsNativeTick())
        {
            g_hairGated.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const bool previous = t_inHair;
        if (hair)
        {
            const uint32_t chains = *reinterpret_cast<uint32_t*>(hair + kHairChainCountOffset);
            RecordChainCount(chains);

            // Only the one instance that needs the fixed step gets it. Applying
            // it to every hair instance puts the others on a 60 Hz clock while
            // the rest of the frame runs at the real rate, and they visibly come
            // apart. Other characters' hair and jewellery are separate instances
            // with their own chain counts and are left on the normal path.
            t_inHair = chains == static_cast<uint32_t>(config::Get().hairFixedStepChainCount);
        }

        g_hairSimulationUpdate.unsafe_call(hair);
        t_inHair = previous;
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

        // Cloth has to advance at the rate of whatever moves its anchor points,
        // not at the frame rate. During a cutscene the animation driving those
        // anchors is gated to 60 Hz, so simulating cloth at the full frame rate
        // takes several steps against anchors that have not moved and then
        // lurches when they jump. That is visible as garments flapping and
        // rippling in cutscenes while behaving correctly in gameplay.
        //
        // Gating cloth here is not the same mistake as gating it in gameplay.
        // There it put cloth out of step with everything around it; here it puts
        // cloth in step with the only thing that actually drives it.
        const bool followCutscene =
            config::Get().clothFollowsCutscene && mgs4::IsCutsceneAdvancing();

        if (followCutscene)
        {
            if (!IsNativeTick())
            {
                g_producerGated.fetch_add(1, std::memory_order_relaxed);
                if (producer && g_clothTransformPublish)
                    g_clothTransformPublish(producer);
                return;
            }

            const bool previous = t_inClothProducer;
            t_inClothProducer = true;
            g_clothProducerUpdate.unsafe_call(producer, updateArgument, updateType);
            t_inClothProducer = previous;
            return;
        }

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
        {"hairSimulationUpdate", kHairSimulationUpdateSignature, nullptr},
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
    g_hairSimulationUpdate = safetyhook::create_inline(
        targets[4].address, reinterpret_cast<void*>(&HairSimulationUpdateHook));

    if (!g_spursTaskTiming || !g_clothManagerUpdate || !g_clothProducerUpdate ||
        !g_hairSimulationUpdate)
    {
        logging::Error("cloth: one or more hooks failed to install");
        g_spursTaskTiming = {};
        g_clothManagerUpdate = {};
        g_clothProducerUpdate = {};
        g_hairSimulationUpdate = {};
        g_clothTransformPublish = nullptr;
        return false;
    }

    logging::Info("cloth: task timing and cloth gating installed");
    return true;
}

void mgs4::LogClothCounters(double intervalSeconds)
{
    // Rates, not running totals. What matters when diagnosing a scene is how
    // often each system ran per second, because a system that disagrees with
    // the frame rate is the one out of step with everything else.
    static uint64_t lastProducer = 0, lastGated = 0, lastHair = 0, lastSubstituted = 0;

    const uint64_t producer = g_producerCalls.load(std::memory_order_relaxed);
    const uint64_t gated = g_producerGated.load(std::memory_order_relaxed);
    const uint64_t hair = g_hairCalls.load(std::memory_order_relaxed);
    const uint64_t substituted = g_stepsSubstituted.load(std::memory_order_relaxed);

    const auto rate = [intervalSeconds](uint64_t now, uint64_t& last) {
        const double per = intervalSeconds > 0.0
                               ? static_cast<double>(now - last) / intervalSeconds
                               : 0.0;
        last = now;
        return per;
    };

    const double producerRate = rate(producer, lastProducer);
    const double gatedRate = rate(gated, lastGated);
    static uint64_t lastHairGated = 0;
    const double hairRate = rate(hair, lastHair);
    const double hairGatedRate = rate(g_hairGated.load(std::memory_order_relaxed), lastHairGated);
    const double substitutedRate = rate(substituted, lastSubstituted);

    const float delta = g_frameDeltaSeconds ? *g_frameDeltaSeconds : 0.0f;
    const double fps = delta > 0.0f ? 1.0 / delta : 0.0;

    if (producer == 0 && hair == 0)
        return;

    logging::Info("survey: frame {:.0f}/s | cloth {:.0f}/s (gated {:.0f}/s) | "
                  "hair {:.0f}/s (gated {:.0f}/s) | task steps substituted {:.0f}/s",
                  fps, producerRate - gatedRate, gatedRate, hairRate - hairGatedRate,
                  hairGatedRate, substitutedRate);

    std::string counts;
    for (const auto& slot : g_seenChainCounts)
    {
        const uint32_t value = slot.load(std::memory_order_relaxed);
        if (value == 0)
            break;
        if (!counts.empty())
            counts += ", ";
        counts += std::to_string(value);
    }
    if (!counts.empty())
    {
        logging::Info("survey: hair chain counts present: {} (fixed step applied to {})", counts,
                      config::Get().hairFixedStep
                          ? std::to_string(config::Get().hairFixedStepChainCount)
                          : std::string("none"));
    }
}

void mgs4::EnterJacketScope(bool active)
{
    t_inJacket = active;
}
