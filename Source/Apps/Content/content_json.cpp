#include "content_json.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace hs::content
{

[[noreturn]] void ThrowValidationError(std::string_view file, std::string_view path,
                                       std::string_view message)
{
    throw std::runtime_error(std::string(file) + ":" + std::string(path) + ": " +
                             std::string(message));
}

const Json &RequireMember(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    if (!object.is_object())
    {
        ThrowValidationError(file, path, "expected object");
    }
    const auto iterator = object.find(key);
    if (iterator == object.end())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "missing member");
    }
    return *iterator;
}

const Json &RequireArray(const Json &object, std::string_view file,
                         std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_array())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected array");
    }
    return value;
}

std::string RequireString(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
             "expected non-empty string");
    }
    return value.get<std::string>();
}

double RequireNumber(const Json &object, std::string_view file,
                     std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_number())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number))
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
             "number must be finite");
    }
    return number;
}

std::int64_t RequireInteger(const Json &object, std::string_view file,
                            std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_number_integer())
    {
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected integer");
    }
    return value.get<std::int64_t>();
}

void RequireExactKeys(const Json &object, std::string_view file,
                      std::initializer_list<std::string_view> expected)
{
    if (!object.is_object())
        ThrowValidationError(file, "$", "expected object");
    for (const auto &[key, unused] : object.items())
    {
        static_cast<void>(unused);
        if (std::ranges::find(expected, key) == expected.end())
        {
            ThrowValidationError(file, std::string("$/") + key, "unexpected root member");
        }
    }
    std::optional<std::string_view> missing;
    for (const auto key : expected)
        if (object.find(key) == object.end() && (!missing || key < *missing))
            missing = key;
    if (missing)
    {
        ThrowValidationError(file, std::string("$/") + std::string(*missing), "missing root member");
    }
}

} // namespace hs::content
