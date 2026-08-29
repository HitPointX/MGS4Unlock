#include "mgs4/camera.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"

// Camera turn rate.
//
// The camera settles towards where the stick is pointing by exponential
// smoothing. Both axes are stepped the same way, once per call:
//
//     target  = input * 5.859375
//     current = current + (target - current) * (1/3)
//
// There is no frame time anywhere in that. The smoothing factor is a constant
// per call, so the camera converges once per frame regardless of how long the
// frame was. At 60 fps that is the intended feel. At 240 the same step runs four
// times as often, so the camera reaches the target roughly four times sooner,
// which reads as the stick being far too sensitive. It is most obvious where the
// camera is meant to move slowly and deliberately, such as crawling through a
// duct.
//
// The framerate independent form of that smoothing is
//
//     factor = 1 - (1 - 1/3) ^ (delta * 60)
//
// which is 1/3 at 60 fps and about 0.096 at 240, so the camera covers the same
// ground per unit of time at any framerate.
//
// The 1/3 constant lives in the shared constant pool with 78 other references,
// so it cannot simply be patched. Instead the correction works on the result:
// the step is linear in its factor, so rescaling the change the original made by
// the ratio of the corrected factor to the stock one lands on exactly the value
// a correctly stepped smoothing would have produced. That has two useful
// properties. It never needs to know the target or either constant beyond the
// factor itself, and when the update takes its early out and changes nothing the
// rescaled change is zero, so the correction is inert rather than wrong.
//
// Two functions run this step. One is the general camera update; the other is a
// small leaf function carrying no unwind data, which is the one that drives the
// confined crawl spaces. Both get the same treatment.

namespace
{
    // The general camera update.
    constexpr const char* kCameraUpdateSignature =
        "48 89 5C 24 10 56 48 83 EC 30 48 8B D9";

    // The leaf variant used by the confined crawl camera. Reached only by direct
    // call, so it carries no unwind record and does not appear in .pdata.
    constexpr const char* kCrawlCameraUpdateSignature =
        "F6 81 30 01 00 00 20 48 8B D1 0F 57 C0";

    // The two smoothed axes, written by the step above.
    constexpr ptrdiff_t kTurnYawOffset = 0x194;
    constexpr ptrdiff_t kTurnPitchOffset = 0x198;

    // The stock per-call smoothing factor.
    constexpr float kStockSmoothingFactor = 1.0f / 3.0f;

    constexpr float kReferenceFps = 60.0f;

    // Guards against a nonsensical frame delta, such as the first frame or a
    // stall while a level streams in.
    constexpr float kMaximumReasonableDelta = 0.2f;

    SafetyHookInline g_cameraUpdate{};
    SafetyHookInline g_crawlCameraUpdate{};
    const float* g_frameDeltaSeconds = nullptr;

    std::atomic<uint64_t> g_calls{0};
    std::atomic<uint64_t> g_crawlCalls{0};
    std::atomic<uint64_t> g_corrected{0};

    // The factor to rescale one smoothing step by, or 1 to leave it alone.
    float SmoothingCorrection()
    {
        if (!g_frameDeltaSeconds || !config::Get().fixCameraTurnRate)
            return 1.0f;

        const float delta = *g_frameDeltaSeconds;
        if (!std::isfinite(delta) || delta <= 0.0f || delta > kMaximumReasonableDelta)
            return 1.0f;

        // How much of a 60 Hz frame this frame was. One at 60 fps.
        const float frames = delta * kReferenceFps;
        if (frames >= 1.0f)
            return 1.0f;

        const float corrected =
            1.0f - std::pow(1.0f - kStockSmoothingFactor, frames);
        return corrected / kStockSmoothingFactor;
    }

    // Rescales the change the original step made to one axis.
    void RescaleAxis(uint8_t* camera, ptrdiff_t offset, float before, float correction)
    {
        auto* value = reinterpret_cast<float*>(camera + offset);
        const float after = *value;
        if (!std::isfinite(before) || !std::isfinite(after))
            return;

        *value = before + (after - before) * correction;
    }

    void ApplyCorrection(uint8_t* camera, float yawBefore, float pitchBefore)
    {
        const float correction = SmoothingCorrection();
        if (correction == 1.0f)
            return;

        RescaleAxis(camera, kTurnYawOffset, yawBefore, correction);
        RescaleAxis(camera, kTurnPitchOffset, pitchBefore, correction);
        g_corrected.fetch_add(1, std::memory_order_relaxed);
    }

    void __fastcall CameraUpdateHook(uint8_t* camera)
    {
        if (!camera)
        {
            g_cameraUpdate.unsafe_call(camera);
            return;
        }

        const float yawBefore = *reinterpret_cast<float*>(camera + kTurnYawOffset);
        const float pitchBefore = *reinterpret_cast<float*>(camera + kTurnPitchOffset);

        g_cameraUpdate.unsafe_call(camera);

        g_calls.fetch_add(1, std::memory_order_relaxed);
        ApplyCorrection(camera, yawBefore, pitchBefore);
    }

    void __fastcall CrawlCameraUpdateHook(uint8_t* camera)
    {
        if (!camera)
        {
            g_crawlCameraUpdate.unsafe_call(camera);
            return;
        }

        const float yawBefore = *reinterpret_cast<float*>(camera + kTurnYawOffset);
        const float pitchBefore = *reinterpret_cast<float*>(camera + kTurnPitchOffset);

        g_crawlCameraUpdate.unsafe_call(camera);

        g_crawlCalls.fetch_add(1, std::memory_order_relaxed);
        ApplyCorrection(camera, yawBefore, pitchBefore);
    }

    // Resolves one signature and installs one hook. A camera we cannot find is
    // reported and skipped rather than treated as fatal, since the other one is
    // still worth having.
    bool InstallOne(const char* name, const char* signature, SafetyHookInline& hook, void* target)
    {
        const module_info::Section& text = module_info::Text();
        const size_t matches = memory::CountMatches(text, signature);
        if (matches != 1)
        {
            logging::Error("camera: the {} signature matched {} times, expected 1", name, matches);
            return false;
        }

        uint8_t* update = memory::Scan(text, signature);
        logging::Address(name, reinterpret_cast<uintptr_t>(update));

        hook = safetyhook::create_inline(update, target);
        if (!hook)
        {
            logging::Error("camera: failed to hook {}", name);
            return false;
        }
        return true;
    }
} // namespace

bool mgs4::InstallCameraTiming(const void* frameTimingStruct)
{
    if (!frameTimingStruct)
    {
        logging::Error("camera: the frame timing struct is unavailable");
        return false;
    }

    constexpr ptrdiff_t kFrameDeltaOffset = 0x18;
    g_frameDeltaSeconds =
        reinterpret_cast<const float*>(static_cast<const uint8_t*>(frameTimingStruct) +
                                       kFrameDeltaOffset);

    const bool general = InstallOne("cameraUpdate", kCameraUpdateSignature, g_cameraUpdate,
                                    reinterpret_cast<void*>(&CameraUpdateHook));
    const bool crawl = InstallOne("crawlCameraUpdate", kCrawlCameraUpdateSignature,
                                  g_crawlCameraUpdate,
                                  reinterpret_cast<void*>(&CrawlCameraUpdateHook));

    if (!general && !crawl)
    {
        g_frameDeltaSeconds = nullptr;
        return false;
    }

    logging::Info("camera: turn smoothing normalised to 60 Hz");
    return true;
}

void mgs4::LogCameraCounters(double intervalSeconds)
{
    static uint64_t lastCalls = 0, lastCrawl = 0, lastCorrected = 0;

    const uint64_t calls = g_calls.load(std::memory_order_relaxed);
    const uint64_t crawl = g_crawlCalls.load(std::memory_order_relaxed);
    if (calls == 0 && crawl == 0)
        return;

    const uint64_t corrected = g_corrected.load(std::memory_order_relaxed);
    const auto rate = [intervalSeconds](uint64_t now, uint64_t& last) {
        const double per =
            intervalSeconds > 0.0 ? static_cast<double>(now - last) / intervalSeconds : 0.0;
        last = now;
        return per;
    };

    logging::Info("survey: camera {:.0f}/s, crawl camera {:.0f}/s ({:.0f}/s corrected)",
                  rate(calls, lastCalls), rate(crawl, lastCrawl),
                  rate(corrected, lastCorrected));
}
