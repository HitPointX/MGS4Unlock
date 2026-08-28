#include <windows.h>

#include <filesystem>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"
#include "core/pdata.h"
#include "mgs4/picker.h"
#include "proxy/proxy.h"

namespace
{
    constexpr const char* kVersion = "0.1.0";

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
        if (settings.patchPicker)
        {
            if (!mgs4::PatchFrameratePicker())
                logging::Error("picker: the framerate picker was left unchanged");
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
        return 0;
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
