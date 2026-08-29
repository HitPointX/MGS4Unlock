#include <windows.h>

#include <filesystem>
#include <optional>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"
#include "core/pdata.h"
#include "dumper/dumper.h"
#include "mgs4/character.h"
#include "mgs4/cloth.h"
#include "mgs4/picker.h"
#include "mgs4/savedsettings.h"
#include "mgs4/timing.h"
#include "proxy/proxy.h"

namespace
{
    constexpr const char* kVersion = "0.7d";

    HMODULE g_self = nullptr;

    std::filesystem::path SelfDirectory()
    {
        wchar_t path[MAX_PATH]{};
        if (::GetModuleFileNameW(g_self, path, MAX_PATH) == 0)
            return {};
        return std::filesystem::path(path).remove_filename();
    }

    DWORD WINAPI Main(LPVOID)
    {
        const std::filesystem::path dir = SelfDirectory();

        logging::Open(dir / "logs" / "MGS4Unlock.log");
        logging::Info("MGS4Unlock v{} loaded, impersonating {}.dll", kVersion, PROXY_TARGET_NAME);

        // Forwarding is set up off the loader lock rather than inside DllMain:
        // LoadLibrary under the lock is a deadlock risk, and the only consumer
        // of dbghelp in this process is crash handling, which cannot run before
        // we get here.
        if (!proxy::Initialize())
            logging::Warn("proxy: forwarding is incomplete, the host may misbehave if it calls us");

        if (!module_info::Initialize())
        {
            logging::Error("module: failed to parse the host image, aborting");
            return 1;
        }

        logging::Info("module: base {:#x}, .text {} bytes, .rdata {} bytes, .data {} bytes",
                  module_info::Base(), module_info::Text().size, module_info::RData().size,
                  module_info::Data().size);

        if (!pdata::Initialize())
            logging::Warn("pdata: unavailable, the unpack detector will not work");

        config::Load(dir / "MGS4Unlock.ini");
        const config::Settings& settings = config::Get();

        // The picker table is in .data, so this does not have to wait for the
        // Steam stub. Do it first: it is the one change that must land before
        // the player reaches the options menu.
        int target = 0;
        if (settings.patchPicker)
        {
            if (!mgs4::PatchFrameratePicker())
                logging::Error("picker: the framerate picker was left unchanged");

            // The game saves the chosen rate but never loads it back into its
            // target-framerate path, so a rate above the stock list silently
            // reverts to 60 on the next launch. Restore it here.
            target = settings.targetFramerate;
            if (target == 0)
                target = mgs4::ReadSavedFramerate(dir).value_or(0);

            if (target > 0)
                mgs4::SeedTargetFramerate(target);
        }
        else
        {
            logging::Info("picker: disabled by config");
        }

        // Everything from here needs real instructions to scan, so wait for the
        // DRM stub to decrypt .text.
        if (!module_info::WaitForUnpack(settings.unpackTimeoutMs))
        {
            logging::Error("module: giving up on the timing hooks");
            return 1;
        }

        logging::Info("module: ready for code scanning");

        if (settings.dumpSections)
            dumper::DumpSections(dir / "dump");

        // The seed above only holds until the game next re-applies its
        // framerate config value, which runs it back through a clamp that snaps
        // anything above 60 straight back down. Bypass that clamp for our one
        // target value so the seed actually stays put.
        if (target > 0)
        {
            if (!mgs4::InstallFramerateClampBypass(target))
                logging::Error("picker: the framerate may revert to 60 after the next config reapply");
        }

        // Extending past three options patches code, so it has to wait for the
        // DRM stub as well.
        if (settings.patchPicker)
        {
            if (!mgs4::InstallExtendedPicker())
                logging::Error("picker: the menu will keep its stock three options");
        }

        if (settings.gateCutscenes)
        {
            if (!mgs4::InstallCutsceneTimingFix())
                logging::Error("timing: cutscenes will run fast above 60 fps");
        }
        else
        {
            logging::Info("timing: cutscene gating disabled by config");
        }

        // Character timing also depends on the frame timing struct.
        if (settings.fixCharacterTiming)
        {
            if (!mgs4::InstallCharacterTiming(mgs4::FrameTimingStruct()))
                logging::Error("character: animation will run slow above 60 fps");
        }
        else
        {
            logging::Info("character: character timing disabled by config");
        }

        // Cloth timing depends on the frame timing globals the cutscene fix
        // resolves, so it has to come after it.
        if (settings.gateCloth)
        {
            if (!mgs4::InstallClothTiming(mgs4::FrameTimingStruct(), mgs4::FrameTickDelta60()))
                logging::Error("cloth: cloth will sway fast above 60 fps");
        }
        else
        {
            logging::Info("cloth: cloth timing disabled by config");
        }

        if (settings.clothDiagnostics)
            mgs4::InstallClothDiagnostics();

        // The target framerate can be reset by more than one code path -- the
        // clamp bypass above covers the config-apply route, but at least one
        // other path sets it directly. Reasserting frequently catches drift
        // from any of them within a second or two rather than requiring every
        // writer to be individually found and hooked.
        //
        // When TargetFramerate is 0 (follow the menu), the desired value is not
        // fixed: the player can pick a different rate and apply it mid-session,
        // which updates the saved-settings file immediately. Re-reading it each
        // tick, rather than only once at startup, is what lets a live menu
        // change take effect instead of being fought back to the old value.
        const bool followMenu = settings.targetFramerate == 0;
        unsigned tick = 0;
        for (;;)
        {
            ::Sleep(1000);
            ++tick;

            if (followMenu)
            {
                if (const std::optional<int> live = mgs4::ReadSavedFramerate(dir);
                    live && *live > 0 && *live != target)
                {
                    logging::Info("picker: live selection changed from {} to {}", target, *live);
                    target = *live;
                    mgs4::SetFramerateClampAllowedValue(target);
                }
            }

            if (target > 0 && settings.patchPicker)
                mgs4::ReassertTargetFramerate(target);

            // Reported as rates over the interval rather than running totals.
            // A system whose rate does not match the frame rate is the one out
            // of step, and that is what identifies a timing problem in a scene.
            if (tick % settings.surveyIntervalSeconds == 0)
            {
                const double interval = static_cast<double>(settings.surveyIntervalSeconds);
                mgs4::LogCharacterCounters(interval);
                mgs4::LogTimingCounters(interval);
                mgs4::LogClothCounters(interval);
            }

            if (tick % 30 == 0 && settings.patchPicker)
                mgs4::ReassertFrameratePicker();
        }
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_self = module;
        ::DisableThreadLibraryCalls(module);
        if (HANDLE thread = ::CreateThread(nullptr, 0, &Main, nullptr, 0, nullptr))
            ::CloseHandle(thread);
        break;

    case DLL_PROCESS_DETACH:
        logging::Close();
        break;

    default:
        break;
    }

    return TRUE;
}
