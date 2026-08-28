#include "core/module.h"

#include <windows.h>

#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/pdata.h"

namespace
{
    // The preferred image base recorded in mgs4.exe's optional header. RVAs we
    // note down from static analysis of the file are relative to this.
    constexpr uintptr_t kPreferredBase = 0x140000000;

    // A page's worth of samples is plenty to characterise the whole section and
    // keeps the poll cheap enough to run every 100 ms.
    constexpr size_t kSampleStride = 4096;

    // Encrypted .text measures ~0.0000 here; genuine MSVC output measures well
    // above 0.01 (mgs4.exe's own .rdata-adjacent code sits around 0.03).
    constexpr double kUnpackedThreshold = 0.005;

    uintptr_t g_base = 0;
    std::vector<std::pair<std::string, module_info::Section>> g_sections;
    module_info::Section g_text, g_rdata, g_data;
} // namespace

bool module_info::Initialize()
{
    const HMODULE host = ::GetModuleHandleW(nullptr);
    if (!host)
        return false;

    g_base = reinterpret_cast<uintptr_t>(host);
    auto* bytes = reinterpret_cast<uint8_t*>(host);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        char name[9] = {};
        std::memcpy(name, section->Name, 8);
        g_sections.emplace_back(name, Section{bytes + section->VirtualAddress,
                                              section->Misc.VirtualSize,
                                              section->SizeOfRawData});
    }

    g_text = Find(".text");
    g_rdata = Find(".rdata");
    g_data = Find(".data");
    return g_text.valid();
}

uintptr_t module_info::Base()
{
    return g_base;
}

module_info::Section module_info::Find(std::string_view name)
{
    for (const auto& [sectionName, section] : g_sections)
    {
        if (sectionName == name)
            return section;
    }
    return {};
}

const module_info::Section& module_info::Text()
{
    return g_text;
}

const module_info::Section& module_info::RData()
{
    return g_rdata;
}

const module_info::Section& module_info::Data()
{
    return g_data;
}

uint8_t* module_info::FromRva(uintptr_t rva)
{
    if (g_base == 0)
        return nullptr;

    // Static analysis is done against the preferred base, but note down RVAs
    // either way: accept both a bare RVA and a full preferred-base address.
    if (rva >= kPreferredBase)
        rva -= kPreferredBase;

    return reinterpret_cast<uint8_t*>(g_base + rva);
}

double module_info::PaddingDensity()
{
    if (!g_text.valid())
        return 0.0;

    size_t samples = 0;
    size_t padded = 0;

    for (size_t offset = 0; offset + 4 <= g_text.size; offset += kSampleStride)
    {
        const uint8_t* p = g_text.begin + offset;
        ++samples;
        if (p[0] == 0xCC && p[1] == 0xCC && p[2] == 0xCC && p[3] == 0xCC)
            ++padded;
    }

    return samples ? static_cast<double>(padded) / static_cast<double>(samples) : 0.0;
}

namespace
{
    // Logs what .text actually holds at a known function start, so a failure to
    // detect the unpack is diagnosable from the log alone rather than guessed at.
    void LogTextSample()
    {
        const module_info::Section& text = module_info::Text();
        if (!text.valid())
            return;

        std::string bytes;
        for (size_t i = 0; i < 16; ++i)
            bytes += std::format("{:02X} ", text.begin[i]);
        logging::Info("unpack: .text[0..16] = {}", bytes);

        const auto functions = pdata::Functions();
        if (functions.empty())
            return;

        const auto& fn = functions[functions.size() / 2];
        const uint8_t* start = module_info::FromRva(fn.beginRva);
        if (!text.contains(start))
            return;

        bytes.clear();
        for (size_t i = 0; i < 16; ++i)
            bytes += std::format("{:02X} ", start[i]);
        logging::Info("unpack: function at rva {:#x} = {}", fn.beginRva, bytes);
    }
} // namespace

bool module_info::WaitForUnpack(unsigned timeoutMs)
{
    constexpr unsigned kPollMs = 100;
    // Random bytes land in the plausible-prologue set about 8% of the time, so
    // 0.80 sits far from both failure modes.
    constexpr double kAgreementThreshold = 0.80;

    bool announced = false;

    for (unsigned waited = 0; waited <= timeoutMs; waited += kPollMs)
    {
        const double agreement = pdata::PrologueAgreement();
        if (agreement > kAgreementThreshold)
        {
            logging::Info("unpack: .text decrypted after {} ms (prologue agreement {:.3f}, "
                          "int3 padding {:.4f})",
                          waited, agreement, PaddingDensity());
            LogTextSample();
            return true;
        }

        if (!announced)
        {
            logging::Info("unpack: waiting for the Steam stub to decrypt .text "
                          "(prologue agreement {:.3f})",
                          agreement);
            LogTextSample();
            announced = true;
        }

        ::Sleep(kPollMs);
    }

    logging::Error("unpack: .text still looks encrypted after {} ms (prologue agreement {:.3f})",
                   timeoutMs, pdata::PrologueAgreement());
    LogTextSample();
    return false;
}
