#include "core/log.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>

#include "core/module.h"

std::string narrow(std::wstring_view text)
{
    if (text.empty())
        return {};

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

namespace
{
    std::mutex g_mutex;
    std::FILE* g_file = nullptr;
} // namespace

void logging::Open(const std::filesystem::path& file)
{
    std::lock_guard lock(g_mutex);
    if (g_file)
        return;

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    g_file = _wfopen(file.c_str(), L"w");
}

void logging::Close()
{
    std::lock_guard lock(g_mutex);
    if (g_file)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void logging::Write(char level, std::string_view message)
{
    std::lock_guard lock(g_mutex);
    if (!g_file)
        return;

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_s(&tm, &tt);

    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] [%c] %.*s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 static_cast<int>(ms.count()), level, static_cast<int>(message.size()),
                 message.data());
    // Flushed every line: if the game hard-crashes we still want the last entry.
    std::fflush(g_file);
}

void logging::Address(std::string_view name, uintptr_t address)
{
    if (address == 0)
    {
        Write('E', std::format("{} = NOT FOUND", name));
        return;
    }

    const uintptr_t base = module_info::Base();
    if (address < base)
    {
        Write('W', std::format("{} = {:#x} (outside the module, base {:#x})", name, address, base));
        return;
    }

    Write('D', std::format("{} = base+{:#x}", name, address - base));
}
