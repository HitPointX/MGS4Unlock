#include "core/pdata.h"

#include <windows.h>

#include <algorithm>
#include <array>

#include "core/log.h"
#include "core/module.h"

namespace
{
    std::span<const pdata::RuntimeFunction> g_functions;

    // Bytes that can legitimately start an MSVC /O2 x86-64 function. Almost all
    // of these are prologue idioms: REX prefixes, register pushes, the
    // sub rsp / mov rax,rsp forms, and the tail-call and short-leaf shapes.
    constexpr std::array<uint8_t, 24> kPlausibleFirstBytes = {
        0x40, 0x41, 0x44, 0x45, 0x48, 0x49, 0x4C, 0x4D, // REX-prefixed
        0x53, 0x55, 0x56, 0x57,                         // push rbx/rbp/rsi/rdi
        0x83, 0x84, 0x85, 0x89, 0x8B, 0x8D,             // cmp/test/mov/lea
        0x0F, 0x33, 0x31, 0xB8, 0xC3, 0xE9,             // two-byte, xor, mov imm, ret, jmp
    };

    bool IsPlausiblePrologue(uint8_t first)
    {
        return std::find(kPlausibleFirstBytes.begin(), kPlausibleFirstBytes.end(), first) !=
               kPlausibleFirstBytes.end();
    }
} // namespace

bool pdata::Initialize()
{
    auto* bytes = reinterpret_cast<uint8_t*>(module_info::Base());
    if (!bytes)
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(RuntimeFunction))
    {
        logging::Error("pdata: the module has no exception directory");
        return false;
    }

    g_functions = {reinterpret_cast<const RuntimeFunction*>(bytes + dir.VirtualAddress),
                   dir.Size / sizeof(RuntimeFunction)};

    logging::Info("pdata: {} function records, first at rva {:#x}, last at rva {:#x}",
                  g_functions.size(), g_functions.front().beginRva, g_functions.back().beginRva);
    return true;
}

std::span<const pdata::RuntimeFunction> pdata::Functions()
{
    return g_functions;
}

double pdata::PrologueAgreement(size_t sampleCount)
{
    if (g_functions.empty())
        return 0.0;

    const module_info::Section& text = module_info::Text();
    if (!text.valid())
        return 0.0;

    // Spread the samples evenly across the whole section rather than clustering
    // at the start: a partially decrypted image would otherwise read as done.
    const size_t stride = std::max<size_t>(1, g_functions.size() / sampleCount);

    size_t sampled = 0;
    size_t plausible = 0;

    for (size_t i = 0; i < g_functions.size(); i += stride)
    {
        uint8_t* start = module_info::FromRva(g_functions[i].beginRva);
        if (!text.contains(start))
            continue;

        ++sampled;
        if (IsPlausiblePrologue(*start))
            ++plausible;
    }

    return sampled ? static_cast<double>(plausible) / static_cast<double>(sampled) : 0.0;
}
