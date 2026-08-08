#include <hs/runtime/experiment_pipe.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <limits>
#include <utility>

namespace hs
{
namespace
{

constexpr DWORD kMaximumMessageBytes = 64 * 1024;

class Handle
{
  public:
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

  private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] Result Failure(std::string_view operation, DWORD error = GetLastError())
{
    return Result::Failure(ErrorCode::InvalidState, "hs_experiment_pipe",
                           std::format("{} failed with Win32 error {}.", operation, error));
}

[[nodiscard]] std::wstring PipePath(std::wstring_view name)
{
    std::wstring path(L"\\\\.\\pipe\\ProjectHS.Experiment.");
    path.append(name);
    return path;
}

[[nodiscard]] bool ValidName(std::wstring_view name) noexcept
{
    return !name.empty() && name.size() <= 64 &&
           std::ranges::all_of(name, [](wchar_t value) {
               return (value >= L'a' && value <= L'z') ||
                      (value >= L'A' && value <= L'Z') ||
                      (value >= L'0' && value <= L'9') || value == L'-' || value == L'_';
           });
}

} // namespace

Result ParseExperimentPipeCommand(std::string_view text,
                                  ExperimentPipeCommand &command)
{
    try
    {
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object() || json.size() != 4 || !json.contains("sequence") ||
            !json.contains("target_tick") || !json.contains("command") ||
            !json.contains("payload") || !json.at("sequence").is_number_unsigned() ||
            !json.at("target_tick").is_number_unsigned() ||
            !json.at("command").is_string() || !json.at("payload").is_object())
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_experiment_pipe",
                                   "Command must contain sequence, target_tick, command, payload.");
        }
        ExperimentPipeCommand parsed;
        parsed.sequence = json.at("sequence").get<Sequence>();
        parsed.target_tick = json.at("target_tick").get<Tick>();
        parsed.command = json.at("command").get<std::string>();
        parsed.payload_json = json.at("payload").dump();
        if (parsed.sequence == 0 || parsed.command.empty() || parsed.command.size() > 64)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_experiment_pipe",
                                   "Command sequence/name is invalid.");
        }
        command = std::move(parsed);
        return Result::Success();
    }
    catch (const std::exception &exception)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_experiment_pipe",
                               exception.what());
    }
}

std::string BuildExperimentPipeReply(Sequence sequence, Tick accepted_tick,
                                     std::string_view status, std::string_view result)
{
    return nlohmann::json{{"sequence", sequence},
                          {"accepted_tick", accepted_tick},
                          {"status", status},
                          {"result", result}}
        .dump();
}

Result ExperimentPipeCommandQueue::Handle(std::string_view request, Tick current_tick,
                                          std::string &reply)
{
    ExperimentPipeCommand command;
    if (auto parsed = ParseExperimentPipeCommand(request, command); !parsed)
    {
        reply = BuildExperimentPipeReply(0, current_tick, "rejected", parsed.Message());
        return Result::Success();
    }

    std::lock_guard lock(mutex_);
    if (const auto previous = replies_.find(command.sequence); previous != replies_.end())
    {
        reply = previous->second;
        return Result::Success();
    }
    if (command.target_tick <= current_tick)
    {
        reply = BuildExperimentPipeReply(command.sequence, current_tick, "rejected",
                                         "missed_target_tick");
        replies_.emplace(command.sequence, reply);
        return Result::Success();
    }
    reply = BuildExperimentPipeReply(command.sequence, current_tick, "accepted", "queued");
    replies_.emplace(command.sequence, reply);
    accepted_.push_back(std::move(command));
    return Result::Success();
}

std::vector<ExperimentPipeCommand> ExperimentPipeCommandQueue::TakeAccepted()
{
    std::lock_guard lock(mutex_);
    auto commands = std::move(accepted_);
    accepted_.clear();
    std::ranges::sort(commands, [](const auto &left, const auto &right) {
        return left.target_tick < right.target_tick ||
               (left.target_tick == right.target_tick && left.sequence < right.sequence);
    });
    return commands;
}

std::size_t ExperimentPipeCommandQueue::AcceptedCount() const
{
    std::lock_guard lock(mutex_);
    return accepted_.size();
}

Result ServeExperimentPipeOnce(std::wstring_view name,
                               const ExperimentPipeHandler &handler)
{
    if (!ValidName(name) || !handler)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_experiment_pipe",
                               "Pipe name/handler is invalid.");
    }
    const auto path = PipePath(name);
    Handle pipe(CreateNamedPipeW(
        path.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, kMaximumMessageBytes, kMaximumMessageBytes, 0, nullptr));
    if (pipe.Get() == INVALID_HANDLE_VALUE) return Failure("CreateNamedPipeW");
    if (!ConnectNamedPipe(pipe.Get(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        return Failure("ConnectNamedPipe");

    std::array<char, kMaximumMessageBytes> request{};
    DWORD read{};
    if (!ReadFile(pipe.Get(), request.data(), static_cast<DWORD>(request.size()), &read,
                  nullptr))
        return Failure("ReadFile(pipe)");
    std::string reply;
    if (auto handled = handler(std::string_view(request.data(), read), reply); !handled)
        reply = BuildExperimentPipeReply(0, 0, "rejected", handled.Message());
    if (reply.size() > kMaximumMessageBytes)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_experiment_pipe",
                               "Pipe reply exceeds 64 KiB.");
    }
    DWORD written{};
    if (!WriteFile(pipe.Get(), reply.data(), static_cast<DWORD>(reply.size()), &written,
                   nullptr) || written != reply.size())
        return Failure("WriteFile(pipe)");
    FlushFileBuffers(pipe.Get());
    DisconnectNamedPipe(pipe.Get());
    return Result::Success();
}

Result TransactExperimentPipe(std::wstring_view name, std::string_view request,
                              std::string &reply, std::chrono::milliseconds timeout)
{
    if (!ValidName(name) || request.empty() || request.size() > kMaximumMessageBytes ||
        timeout.count() < 0 || timeout.count() > std::numeric_limits<DWORD>::max())
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_experiment_pipe",
                               "Pipe transaction arguments are invalid.");
    }
    std::array<char, kMaximumMessageBytes> response{};
    DWORD read{};
    const auto path = PipePath(name);
    if (!CallNamedPipeW(path.c_str(), const_cast<char *>(request.data()),
                        static_cast<DWORD>(request.size()), response.data(),
                        static_cast<DWORD>(response.size()), &read,
                        static_cast<DWORD>(timeout.count())))
        return Failure("CallNamedPipeW");
    reply.assign(response.data(), read);
    return Result::Success();
}

} // namespace hs
