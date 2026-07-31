#pragma once

#include <cstdint>
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
        return subsystem_;
    }

    [[nodiscard]] std::string_view Message() const noexcept
    {
        return message_;
    }

    [[nodiscard]] const std::source_location &Location() const noexcept
    {
        return location_;
    }

  private:
    Result() = default;

    Result(ErrorCode code, std::string_view subsystem, std::string message,
           std::source_location location)
        : code_(code), subsystem_(subsystem), message_(std::move(message)), location_(location)
    {
    }

    ErrorCode code_{ErrorCode::None};
    std::string subsystem_;
    std::string message_;
    std::source_location location_;
};

} // namespace hs
