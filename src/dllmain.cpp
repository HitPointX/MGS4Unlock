#include <windows.h>

#include <filesystem>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"
#include "core/pdata.h"
#include "dumper/dumper.h"
#include "mgs4/picker.h"
#include "mgs4/savedsettings.h"
#include "mgs4/timing.h"
#include "proxy/proxy.h"

namespace
{
    constexpr const char* kVersion = "0.6a";

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

        if (settings.gateCutscenes)
        {
            if (!mgs4::InstallCutsceneTimingFix())
                logging::Error("timing: cutscenes will run fast above 60 fps");
        }
        else
        {
            logging::Info("timing: cutscene gating disabled by config");
        }

        if (settings.gateCloth)
        {
            if (!mgs4::InstallClothTimingFix())
                logging::Error("timing: cloth will sway fast above 60 fps");
        }
        else
        {
            logging::Info("timing: cloth gating disabled by config");
        }

        // The target framerate can be reset by more than one code path -- the
        // clamp bypass above covers the config-apply route, but at least one
        // other path sets it directly. Reasserting frequently catches drift
        // from any of them within a second or two rather than requiring every
        // writer to be individually found and hooked.
        unsigned tick = 0;
        for (;;)
        {
            ::Sleep(1000);
            ++tick;

            if (target > 0 && settings.patchPicker)
                mgs4::ReassertTargetFramerate(target);

            if (tick % 30 == 0)
            {
                mgs4::LogTimingCounters();
                if (settings.patchPicker)
                    mgs4::ReassertFrameratePicker();
            }
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
