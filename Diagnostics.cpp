#include "Diagnostics.h"
#include <DbgHelp.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace Diagnostics
{
namespace
{
    std::recursive_mutex mutex;
    std::ofstream logFile;
    std::filesystem::path logPath;
    bool symbolsReady = false;
    volatile LONG reporting = 0;
    thread_local Trace lastThrow;

    Trace Capture() noexcept
    {
        Trace trace;
        trace.count = CaptureStackBackTrace(1, static_cast<DWORD>(trace.frames.size()), trace.frames.data(), nullptr);
        return trace;
    }

    // Observe MSVC C++ throws before unwinding, without handling the exception.
    LONG CALLBACK ObserveThrow(EXCEPTION_POINTERS* exception) noexcept
    {
        if (exception->ExceptionRecord->ExceptionCode == 0xE06D7363)
            lastThrow = Capture();
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void EnsureConsole(bool force)
    {
#ifdef STEAMTOOLS_DIAGNOSTICS_TEST_HEADLESS
        return;
#endif
        if (!GetConsoleWindow() && !AttachConsole(ATTACH_PARENT_PROCESS) && (!force || !AllocConsole()))
            return;
        FILE* stream = nullptr;
        // Preserve stdout/stderr redirection when launched from a terminal or script.
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!output || output == INVALID_HANDLE_VALUE || GetFileType(output) == FILE_TYPE_CHAR)
            freopen_s(&stream, "CONOUT$", "w", stdout);
        const HANDLE errors = GetStdHandle(STD_ERROR_HANDLE);
        if (!errors || errors == INVALID_HANDLE_VALUE || GetFileType(errors) == FILE_TYPE_CHAR)
            freopen_s(&stream, "CONOUT$", "w", stderr);
        std::cout.clear();
        SetConsoleTitleW(L"SteamTools Diagnostics");
    }

    std::string FormatTrace(const Trace& trace)
    {
        std::lock_guard lock(mutex); // All DbgHelp calls must be serialized.
        std::ostringstream out;
        out << "Traceback (most recent call first):\n";
        for (unsigned short i = 0; i < trace.count; ++i)
        {
            const DWORD64 address = reinterpret_cast<DWORD64>(trace.frames[i]);
            out << "  #" << std::dec << i << " 0x" << std::hex << address;
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            if (symbolsReady && SymGetModuleInfo64(GetCurrentProcess(), address, &module))
                out << " " << module.ModuleName << "+0x" << (address - module.BaseOfImage);
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (symbolsReady && SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
                out << " " << symbol->Name << "+0x" << displacement;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (symbolsReady && SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line))
                out << " (" << line.FileName << ':' << std::dec << line.LineNumber << ')';
            out << '\n';
        }
        if (trace.count == 0) out << "  Stack unavailable.\n";
        if (trace.count == trace.frames.size()) out << "  Trace truncated at 128 frames.\n";
        out << "Source lines require matching PDB files; optimized/inlined frames may be unavailable.\n";
        return out.str();
    }

    Trace CaptureContext(CONTEXT context)
    {
        std::lock_guard lock(mutex);
        Trace trace;
        STACKFRAME64 frame{};
#if defined(_M_X64)
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = context.Rip;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
        const DWORD machine = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset = context.Eip;
        frame.AddrFrame.Offset = context.Ebp;
        frame.AddrStack.Offset = context.Esp;
#else
#error Diagnostics supports Windows x64 and x86 builds.
#endif
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        trace.frames[trace.count++] = reinterpret_cast<void*>(frame.AddrPC.Offset);
        DWORD64 previousPC = frame.AddrPC.Offset;
        DWORD64 previousSP = frame.AddrStack.Offset;
        bool firstStep = true;
        while (trace.count < trace.frames.size() && symbolsReady &&
            StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        {
            if (!frame.AddrPC.Offset) break;
            if (frame.AddrPC.Offset == previousPC && frame.AddrStack.Offset == previousSP)
            {
                // StackWalk64's first result can be the fault frame already recorded above.
                if (!firstStep) break;
                firstStep = false;
                continue;
            }
            firstStep = false;
            trace.frames[trace.count++] = reinterpret_cast<void*>(frame.AddrPC.Offset);
            previousPC = frame.AddrPC.Offset;
            previousSP = frame.AddrStack.Offset;
        }
        return trace;
    }

    void DescribeException(std::ostringstream& out, const std::exception& error, unsigned depth = 0)
    {
        out << (depth ? "Caused by: " : "Exception: ") << error.what() << '\n';
        if (const auto* diagnostic = dynamic_cast<const Error*>(&error))
        {
            out << diagnostic->location.file_name() << ':' << diagnostic->location.line()
                << " in " << diagnostic->location.function_name() << '\n';
            out << FormatTrace(diagnostic->trace);
        }
        if (depth >= 16) { out << "Nested exception limit reached.\n"; return; }
        const auto* nested = dynamic_cast<const std::nested_exception*>(&error);
        if (nested && nested->nested_ptr())
        {
            try { nested->rethrow_nested(); }
            catch (const std::exception& inner) { DescribeException(out, inner, depth + 1); }
            catch (...) { out << "Caused by: unknown non-standard exception.\n"; }
        }
    }

    void FinishReport(const std::string& details)
    {
        EnsureConsole(true);
        Log("FATAL", "The application exited unexpectedly.\n" + details);
        std::wstring message = L"The application exited unexpectedly.\n\n";
        if (logFile.is_open() && logFile.good())
            message += L"Error details and a traceback were saved to:\n" + logPath.wstring();
        else
            message += L"The error log could not be written. See the diagnostics console for details.";
#ifndef STEAMTOOLS_DIAGNOSTICS_TEST_HEADLESS
        MessageBoxW(nullptr, message.c_str(), L"SteamTools - Unexpected Exit", MB_OK | MB_ICONERROR | MB_TASKMODAL);
#endif
    }

    void EmergencyReport() noexcept
    {
        const char* message = "The application exited unexpectedly. Crash reporting also failed.\n";
        OutputDebugStringA(message);
        fputs(message, stderr);
#ifndef STEAMTOOLS_DIAGNOSTICS_TEST_HEADLESS
        MessageBoxA(nullptr, message, "SteamTools - Unexpected Exit", MB_OK | MB_ICONERROR);
#endif
    }

    LONG WINAPI UnhandledException(EXCEPTION_POINTERS* exception) noexcept
    {
        if (InterlockedExchange(&reporting, 1)) TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) { EmergencyReport(); return EXCEPTION_EXECUTE_HANDLER; }
        try
        {
            std::ostringstream out;
            const auto* record = exception->ExceptionRecord;
            out << "Windows exception 0x" << std::hex << record->ExceptionCode
                << " at " << record->ExceptionAddress << '\n';
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
                out << "Access violation: operation " << record->ExceptionInformation[0]
                    << " (0=read, 1=write, 8=execute), address 0x" << record->ExceptionInformation[1] << '\n';
            out << FormatTrace(CaptureContext(*exception->ContextRecord));
            FinishReport(out.str());
        }
        catch (...) { EmergencyReport(); }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void TerminateHandler() noexcept
    {
        ExitFromCurrentException("std::terminate (uncaught exception or noexcept violation)");
    }

    void AbortHandler(int) noexcept
    {
        ExitFromCurrentException("SIGABRT (abort/assertion failure)");
    }
}

Error::Error(const std::string& message, std::source_location source)
    : std::runtime_error(message), trace(Capture()), location(source) {}

bool IsReporting() noexcept
{
    return InterlockedCompareExchange(&reporting, 0, 0) != 0;
}

void Initialize()
{
    SetUnhandledExceptionFilter(UnhandledException);
    InstallThreadHandlers();
    // The CRT's Debug abort dialog otherwise appears before our SIGABRT handler.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, AbortHandler);
    AddVectoredExceptionHandler(1, ObserveThrow);
#ifdef _DEBUG
    EnsureConsole(true);
#else
    EnsureConsole(false);
#endif
    wchar_t directory[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, static_cast<DWORD>(std::size(directory)));
    const std::filesystem::path candidates[] = {
        length && length < std::size(directory) ? std::filesystem::path(directory) / L"SteamToolsPort" / L"Logs" : std::filesystem::path(),
        std::filesystem::temp_directory_path() / L"SteamToolsPort" / L"Logs",
        std::filesystem::current_path() / L"Logs"
    };
    for (const auto& candidate : candidates)
    {
        if (candidate.empty()) continue;
        std::error_code error;
        std::filesystem::create_directories(candidate, error);
        if (error) continue;
        logPath = candidate / L"application.log";
        logFile.clear();
        logFile.open(logPath, std::ios::app);
        if (logFile) break;
    }
    {
        std::lock_guard lock(mutex);
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        // Include the executable directory even when launched from another working directory.
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        const auto searchPath = std::filesystem::path(executable).parent_path().wstring();
        symbolsReady = SymInitializeW(GetCurrentProcess(), searchPath.c_str(), TRUE) != FALSE;
    }
    Log("INFO", "Application starting.");
    if (!logFile) Log("ERROR", "Unable to open a persistent error log.");
    if (!symbolsReady) Log("WARNING", "DbgHelp initialization failed; traceback addresses remain available.");
}

void InstallThreadHandlers() noexcept
{
    std::set_terminate(TerminateHandler);
}

void Log(const char* level, const std::string& message) noexcept
{
    try
    {
        std::lock_guard lock(mutex);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::ostringstream out;
        out << '[' << time.wYear << '-' << std::setfill('0') << std::setw(2) << time.wMonth << '-'
            << std::setw(2) << time.wDay << ' ' << std::setw(2) << time.wHour << ':'
            << std::setw(2) << time.wMinute << ':' << std::setw(2) << time.wSecond << '.'
            << std::setw(3) << time.wMilliseconds << "] [PID " << GetCurrentProcessId()
            << ", thread " << GetCurrentThreadId() << "] [" << level << "] " << message << '\n';
        const auto text = out.str();
        std::cout << text << std::flush;
        OutputDebugStringA(text.c_str());
        if (logFile.is_open()) { logFile << text; logFile.flush(); }
    }
    catch (...) { OutputDebugStringA("SteamTools: unable to write diagnostics.\n"); }
}

void CheckHRESULT(HRESULT result, const char* operation, std::source_location location)
{
    if (SUCCEEDED(result)) return;
    char message[1024]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(result), 0, message, static_cast<DWORD>(std::size(message)), nullptr);
    std::ostringstream out;
    out << operation << " failed: HRESULT 0x" << std::hex << std::uppercase
        << static_cast<unsigned long>(result) << " " << message;
    throw Error(out.str(), location);
}

void ReportCurrentException(const char* boundary) noexcept
{
    if (InterlockedExchange(&reporting, 1)) TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) { EmergencyReport(); return; }
    try
    {
        const Trace throwTrace = lastThrow; // Preserve before rethrowing/nested exception inspection.
        std::ostringstream out;
        out << "Boundary: " << boundary << '\n';
        auto exception = std::current_exception();
        if (exception)
        {
            try { std::rethrow_exception(exception); }
            catch (const std::exception& error) { DescribeException(out, error); }
            catch (...) { out << "Unknown non-standard C++ exception.\n"; }
            out << "Last observed C++ throw on this thread:\n" << FormatTrace(throwTrace);
        }
        else out << "No active C++ exception.\n";
        out << "Reporting stack:\n" << FormatTrace(Capture());
        FinishReport(out.str());
    }
    catch (...) { EmergencyReport(); }
}

[[noreturn]] void ExitFromCurrentException(const char* boundary) noexcept
{
    ReportCurrentException(boundary);
    // Do not run ImGui shutdown or wait on a potentially broken GPU after a fatal error.
    ExitProcess(EXIT_FAILURE);
}
}
