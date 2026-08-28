#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

std::string narrow(std::wstring_view text);

namespace logging
{
    // Opens the log file, truncating it. Messages logged before this are dropped.
    void Open(const std::filesystem::path& file);
    void Close();

    void Write(char level, std::string_view message);

    // Logs an address as a module-relative RVA so log lines paste straight into
    // a disassembler at the same image base. Pass 0 to record a failed lookup.
    void Address(std::string_view name, uintptr_t address);

    template <typename... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        Write('I', std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void Warn(std::format_string<Args...> fmt, Args&&... args)
    {
        Write('W', std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void Error(std::format_string<Args...> fmt, Args&&... args)
    {
        Write('E', std::format(fmt, std::forward<Args>(args)...));
    }
} // namespace logging
