#include "../Content/content_cooker.hpp"
#include "../AssetPipeline/action.hpp"

#include <cmath>
#include <set>
#include <stdexcept>

// These are the particle cooker's validation helpers. Keeping this small bridge
// in the standalone executable avoids pulling in global GameData validation.
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
        ThrowValidationError(file, path, "expected object");
    const auto found = object.find(key);
    if (found == object.end())
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "missing member");
    return *found;
}

const Json &RequireArray(const Json &object, std::string_view file,
                         std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_array())
        ThrowValidationError(file, std::string(path) + "/" + std::string(key), "expected array");
    return value;
}

std::string RequireString(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_string() || value.get_ref<const std::string &>().empty())
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
                             "expected non-empty string");
    return value.get<std::string>();
}

double RequireNumber(const Json &object, std::string_view file,
                     std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
                             "expected finite number");
    return value.get<double>();
}

std::int64_t RequireInteger(const Json &object, std::string_view file,
                            std::string_view path, std::string_view key)
{
    const auto &value = RequireMember(object, file, path, key);
    if (!value.is_number_integer())
        ThrowValidationError(file, std::string(path) + "/" + std::string(key),
                             "expected integer");
    return value.get<std::int64_t>();
}

void RequireExactKeys(const Json &object, std::string_view file,
                      std::initializer_list<std::string_view> expected)
{
    if (!object.is_object())
        ThrowValidationError(file, "$", "expected object");
    std::set<std::string, std::less<>> keys;
    for (const auto key : expected)
        keys.emplace(key);
    for (const auto &[key, unused] : object.items())
    {
        static_cast<void>(unused);
        if (!keys.erase(key))
            ThrowValidationError(file, std::string("$/") + key, "unexpected root member");
    }
    if (!keys.empty())
        ThrowValidationError(file, std::string("$/") + *keys.begin(), "missing root member");
}

bool AtomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes,
                 std::string &error_message)
{
    try
    {
        asset_pipeline::WriteBytes(path, bytes);
        return true;
    }
    catch (const std::exception &error)
    {
        error_message = error.what();
        return false;
    }
}
} // namespace hs::content
