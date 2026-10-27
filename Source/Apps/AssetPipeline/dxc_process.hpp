#pragma once

#include "action.hpp"

#include <Windows.h>

#include <stdexcept>

namespace hs::shader_content
{
class DxcStageInUseError final : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

inline DWORD WaitForDxcExit(const PROCESS_INFORMATION &process,
                          const std::filesystem::path &stage,
                          const std::filesystem::path &diagnostics)
{
    const auto wait = WaitForSingleObject(process.hProcess, 120'000);
    if (wait != WAIT_OBJECT_0)
    {
        const auto wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        const BOOL termination_requested = TerminateProcess(process.hProcess, 1);
        const auto terminate_error = termination_requested ? ERROR_SUCCESS : GetLastError();
        const auto exit_wait = WaitForSingleObject(process.hProcess, 10'000);
        const auto exit_wait_error = exit_wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        CloseHandle(process.hProcess);
        if (exit_wait != WAIT_OBJECT_0)
            throw DxcStageInUseError("DXC process exit could not be confirmed after " +
                std::string(wait == WAIT_TIMEOUT ? "timeout" : "wait failure") +
                "; stage preserved: " + stage.string() + " (PID=" + std::to_string(process.dwProcessId) +
                ", termination requested=" + std::to_string(termination_requested != FALSE) +
                ", wait error=" + std::to_string(wait_error) +
                ", termination error=" + std::to_string(terminate_error) +
                ", exit wait=" + std::to_string(exit_wait) +
                ", exit wait error=" + std::to_string(exit_wait_error) + ").");
        throw std::runtime_error("DXC " + std::string(wait == WAIT_TIMEOUT ? "timed out" : "wait failed") + ": " +
            (std::filesystem::exists(diagnostics) ? asset_pipeline::ReadBytes(diagnostics) : "no diagnostic output"));
    }
    DWORD code{};
    const auto got_code = GetExitCodeProcess(process.hProcess, &code);
    const auto exit_error = got_code ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process.hProcess);
    if (!got_code) throw std::runtime_error("cannot read DXC exit code: " + std::to_string(exit_error));
    return code;
}
} // namespace hs::shader_content
