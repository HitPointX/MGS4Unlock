#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

namespace config
{
    // The menu's own list capacity is 30 entries, so this is a self-imposed
    // limit rather than an engine one.
    inline constexpr size_t kMaxPickerValues = 8;

    struct Settings
    {
        // The rates the in-game picker should offer, replacing the stock
        // 30/40/60. The menu formats its labels from these integers, so this is
        // literally what the player sees. More than three is supported: the
        // engine's hardcoded option count is patched to match however many are
        // listed. Kept sorted so the menu reads in ascending order.
        std::array<int, kMaxPickerValues> pickerValues{30, 60, 120, 240};
        size_t pickerValueCount = 4;

        // Framerate to hand the engine at startup. 0 means follow whatever the
        // player last chose in the in-game menu, which the game saves but never
        // reads back into its target-framerate path.
        int targetFramerate = 0;

        // Master switch for the picker patch.
        bool patchPicker = true;

        // Gates cutscene playback to the engine's native 60 Hz tick. Without
        // this, cutscenes play at double speed above 60 fps.
        bool gateCutscenes = true;

        // Corrects character movement and animation speed above 60 fps, by
        // carrying the fractional part of the 300 Hz character tick between
        // frames instead of discarding it.
        bool fixCharacterTiming = true;

        // Corrects cloth timing above 60 fps. Covers the shared simulation
        // task timing as well as the cloth solver itself: gating the solver
        // alone leaves cloth and the rest of the pipeline on different clocks.
        bool gateCloth = true;

        // How cloth itself is kept at the right rate once the shared task
        // timing is corrected. Two genuinely different approaches:
        //
        //   Gate  - cloth keeps its native fixed step and the solver is run
        //           only on native 60 Hz frames. Preserves the exact solver
        //           behaviour it was tuned for, but means cloth state is only
        //           refreshed on some frames.
        //   Delta - cloth is treated like any other task and simulated every
        //           frame with the real frame delta. Nothing is ever skipped,
        //           at the cost of feeding a stiff solver a step it was not
        //           tuned for.
        //
        // Delta is the default, and is what actually works here. Gating cloth
        // produced a doubled, semi-transparent copy of the garment through
        // every variation of what a skipped frame did; simulating every frame
        // with the real delta has none of that and looks correct at 120 and
        // 240. Gate is kept only for comparison.
        enum class ClothMode
        {
            Gate,
            Delta,
        };
        ClothMode clothMode = ClothMode::Delta;

        // While a cutscene is playing, run cloth at the cutscene's rate rather
        // than the frame rate. Cutscene playback is gated to 60 Hz, so the
        // animation moving a garment's anchor points advances at 60 Hz while
        // cloth simulated at the frame rate takes several steps against anchors
        // that have not moved. That is what makes garments flap and ripple in
        // cutscenes while behaving correctly in gameplay.
        bool clothFollowsCutscene = true;

        // Gate the jacket solver to the cutscene rate, and give the frames that
        // do run a fixed 1/60 step. Both halves are needed: gating alone leaves
        // the solver receiving the caller's per-frame delta, so it advances a
        // quarter of the time the body moved and trails the pose, which looks
        // like a doubled jacket.
        bool gateJacket = true;

        // Give the hair solver a fixed step near 1/60 instead of the real frame
        // delta. Snake's bandana floats up off his head without this, because
        // gravity scales down with the step while the chain constraints keep
        // their stiffness, so the chain never settles.
        bool hairFixedStep = true;

        // Which hair instance gets the fixed step, identified by its chain
        // count. Only one instance needs it. Applying it to every instance puts
        // the rest on a 60 Hz clock while the frame runs at the real rate, and
        // other characters' hair and jewellery visibly come apart.
        int hairFixedStepChainCount = 17;

        // On frames where the cloth solver is gated, re-publish the transform
        // the last simulated frame produced. Intended to keep the renderer from
        // drawing stale data, but it is also the remaining suspect for the
        // doubled skirt, so it is switchable.
        bool clothPublishOnSkip = true;

        // Observation-only hooks on the remaining cloth solvers, reporting
        // which instances each one handles. Costs nothing but a counter.
        bool clothDiagnostics = true;

        // How often to write the survey line, in seconds. The survey reports
        // each simulation system's rate, which is how a scene with a timing
        // problem is diagnosed: whichever system is not running at the frame
        // rate, or not at the rate of whatever drives it, is the odd one out.
        unsigned surveyIntervalSeconds = 5;

        // Writes the decrypted sections to disk once the DRM stub has run, for
        // offline analysis. Off by default: it costs ~30 MB per launch.
        bool dumpSections = false;

        // How long to wait for the Steam DRM stub to decrypt .text.
        unsigned unpackTimeoutMs = 30000;
    };

    // Reads the ini next to our DLL, writing a commented default file if none
    // exists. Missing or malformed keys keep their defaults.
    void Load(const std::filesystem::path& file);

    const Settings& Get();
} // namespace config
