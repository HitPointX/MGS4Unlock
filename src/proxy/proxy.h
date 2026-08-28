#pragma once

// The set of exports we must publish for each supported proxy target. These are
// exactly the functions mgs4.exe imports from that DLL -- verified by walking
// the import table of build [Code]a84606af -- so the game's import resolution
// succeeds without us having to mirror the DLL's entire export surface.

#if defined(PROXY_dbghelp)

#define PROXY_TARGET_WIDE L"dbghelp"
#define PROXY_FOREACH_EXPORT(X)                                                \
    X(StackWalk64)                                                             \
    X(SymFunctionTableAccess64)                                                \
    X(SymGetModuleInfo64)                                                      \
    X(MiniDumpWriteDump)                                                       \
    X(SymGetSymFromAddr64)                                                     \
    X(SymInitialize)                                                           \
    X(SymGetLineFromAddr64)                                                    \
    X(SymCleanup)                                                              \
    X(SymGetModuleBase64)

#elif defined(PROXY_winmm)

#define PROXY_TARGET_WIDE L"winmm"
#define PROXY_FOREACH_EXPORT(X)                                                \
    X(timeBeginPeriod)                                                         \
    X(timeGetTime)

#else
#error "PROXY_TARGET must be one of: dbghelp, winmm"
#endif

namespace proxy
{
    // Loads the genuine system DLL and binds every forwarded export to it.
    // Returns false if any export could not be resolved.
    bool Initialize();
} // namespace proxy
