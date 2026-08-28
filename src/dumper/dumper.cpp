#include "dumper/dumper.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/module.h"

// Dumping strategy: write out only the decrypted section *contents*, not a
// reconstructed PE. tools/rebuild_dump.py then splices them into a copy of the
// shipped mgs4.exe at the matching file offsets.
//
// That keeps the in-process work to a plain fwrite -- no header surgery, no
// section-table rewriting, nothing that can go subtly wrong inside the game's
// address space -- while still producing a genuine, fully valid PE for offline
// analysis, because every field other than the section bytes is already correct
// in the shipped file.

namespace
{
    bool DumpSection(const std::filesystem::path& path, const module_info::Section& section,
                     size_t limit)
    {
        if (!section.valid())
            return false;

        const size_t size = limit ? (std::min)(section.size, limit) : section.size;

        std::FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
        {
            logging::Error("dump: could not open {}", narrow(path.wstring()));
            return false;
        }

        const size_t written = std::fwrite(section.begin, 1, size, file);
        std::fclose(file);

        if (written != size)
        {
            logging::Error("dump: short write to {} ({} of {} bytes)", narrow(path.wstring()),
                           written, size);
            return false;
        }

        logging::Info("dump: wrote {} ({} bytes)", narrow(path.filename().wstring()), size);
        return true;
    }
} // namespace

bool dumper::DumpSections(const std::filesystem::path& directory)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    // .data's virtual size is ~575 MB, almost all of it zeroed BSS. Only the
    // portion backed by the file is of any interest, so cap it at the shipped
    // raw size.
    constexpr size_t kDataRawSize = 0x26d200;

    const bool text = DumpSection(directory / "text.bin", module_info::Text(), 0);
    const bool rdata = DumpSection(directory / "rdata.bin", module_info::RData(), 0);
    const bool data = DumpSection(directory / "data.bin", module_info::Data(), kDataRawSize);

    if (std::FILE* manifest = _wfopen((directory / "manifest.txt").c_str(), L"w"))
    {
        std::fprintf(manifest, "base=%llx\n",
                     static_cast<unsigned long long>(module_info::Base()));
        std::fprintf(manifest, "text_size=%zx\n", module_info::Text().size);
        std::fprintf(manifest, "rdata_size=%zx\n", module_info::RData().size);
        std::fprintf(manifest, "data_size=%zx\n", kDataRawSize);
        std::fclose(manifest);
    }

    logging::Info("dump: complete, splice with tools/rebuild_dump.py");
    return text && rdata && data;
}
