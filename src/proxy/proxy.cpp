// Proxy DLL export forwarding.
//
// We ship a DLL named exactly like a system DLL that mgs4.exe imports directly
// (see PROXY_TARGET), so the game's own import table loads us with no ASI loader
// involved. Every export we publish is a bare tail-jump through a function
// pointer that we fill in at load time from the genuine system DLL.
//
// The real DLL is loaded by *full path* out of the system directory. That
// matters: the loader keys modules by resolved path, so
// C:\...\MGS4\dbghelp.dll (us) and C:\windows\system32\dbghelp.dll (the real
// one) are distinct modules and we do not recursively load ourselves.

#include "proxy/proxy.h"

#include <windows.h>

#include "core/log.h"

namespace
{
    HMODULE g_realModule = nullptr;

    // Fallback for any export we could not resolve. Every forwarded function
    // here returns BOOL / DWORD / a pointer, so zero is a sane failure value.
    extern "C" uintptr_t ProxyUnavailable()
    {
        return 0;
    }
} // namespace

// Each forwarded export is a pointer plus a naked tail-jump through it. The
// jump costs one indirect branch and keeps every argument register untouched,
// so it works for any calling convention and any arity.
#define PROXY_EXPORT(name)                                                     \
    extern "C" void* g_real_##name = reinterpret_cast<void*>(&ProxyUnavailable); \
    asm(".text\n"                                                              \
        ".globl " #name "\n"                                                   \
        ".def " #name "; .scl 2; .type 32; .endef\n"                           \
        ".p2align 4\n" #name ":\n"                                             \
        "  jmpq *g_real_" #name "(%rip)\n");

PROXY_FOREACH_EXPORT(PROXY_EXPORT)

#undef PROXY_EXPORT

bool proxy::Initialize()
{
    wchar_t path[MAX_PATH];
    const UINT len = ::GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        logging::Error("proxy: GetSystemDirectoryW failed ({})", ::GetLastError());
        return false;
    }

    ::wcscat_s(path, L"\\" PROXY_TARGET_WIDE L".dll");

    g_realModule = ::LoadLibraryW(path);
    if (!g_realModule)
    {
        logging::Error("proxy: failed to load the real {}.dll ({})", PROXY_TARGET_NAME, ::GetLastError());
        return false;
    }

    if (g_realModule == ::GetModuleHandleW(nullptr))
    {
        logging::Error("proxy: resolved to ourselves, refusing to forward");
        return false;
    }

    int resolved = 0;
    int missing = 0;

#define PROXY_RESOLVE(name)                                                    \
    if (void* addr = reinterpret_cast<void*>(::GetProcAddress(g_realModule, #name))) \
    {                                                                          \
        g_real_##name = addr;                                                  \
        ++resolved;                                                            \
    }                                                                          \
    else                                                                       \
    {                                                                          \
        logging::Warn("proxy: {}.dll has no export {}", PROXY_TARGET_NAME, #name);  \
        ++missing;                                                             \
    }

    PROXY_FOREACH_EXPORT(PROXY_RESOLVE)

#undef PROXY_RESOLVE

    logging::Info("proxy: forwarding to {} ({} exports resolved, {} missing)",
              narrow(path), resolved, missing);
    return missing == 0;
}
