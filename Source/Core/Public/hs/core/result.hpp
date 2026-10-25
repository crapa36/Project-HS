#pragma once

#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace hs
{

enum class ErrorCode : std::uint8_t
{
    None,
    InvalidArgument,
    InvalidState,
    Cancelled,
    TaskFailed,
};

class Result
{
  public:
    [[nodiscard]] static Result Success()
    {
        return {};
    }

    [[nodiscard]] static Result Failure(
        ErrorCode code, std::string_view subsystem, std::string message,
        std::source_location location = std::source_location::current())
    {
        return Result(code, subsystem, std::move(message), location);
    }

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return code_ == ErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return Succeeded();
    }

    [[nodiscard]] ErrorCode Code() const noexcept
    {
        return code_;
    }

    [[nodiscard]] std::string_view Subsystem() const noexcept
    {
        return details_ ? std::string_view{details_->subsystem} : std::string_view{""};
    }

    [[nodiscard]] std::string_view Message() const noexcept
    {
        return details_ ? std::string_view{details_->message} : std::string_view{""};
    }

    [[nodiscard]] const std::source_location &Location() const noexcept
    {
        return location_;
    }

  private:
    struct FailureDetails
    {
        std::string subsystem;
        std::string message;
    };

    Result() = default;

    Result(ErrorCode code, std::string_view subsystem, std::string message,
           std::source_location location)
        : code_(code), details_(std::in_place, std::string{subsystem}, std::move(message)), location_(location)
    {
    }

    ErrorCode code_{ErrorCode::None};
    // Successful calls need no strings (including MSVC Debug iterator allocations).
    std::optional<FailureDetails> details_;
    std::source_location location_;
};

} // namespace hs
