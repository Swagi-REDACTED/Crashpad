#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>

namespace Diagnostics
{
    struct Trace
    {
        std::array<void*, 128> frames{};
        unsigned short count = 0;
    };

    class Error : public std::runtime_error
    {
    public:
        explicit Error(const std::string& message,
            std::source_location location = std::source_location::current());
        Trace trace;
        std::source_location location;
    };

    // Call before creating the window or graphics device.
    void Initialize();
    // MSVC termination handlers are thread-local; call at the start of future worker threads.
    void InstallThreadHandlers() noexcept;
    bool IsReporting() noexcept;
    void Log(const char* level, const std::string& message) noexcept;
    void CheckHRESULT(HRESULT result, const char* operation,
        std::source_location location = std::source_location::current());
    // Must be called from a catch block. Reporting never resumes rendering.
    void ReportCurrentException(const char* boundary) noexcept;
    [[noreturn]] void ExitFromCurrentException(const char* boundary) noexcept;
}
