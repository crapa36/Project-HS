#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace hs::content
{

using Json = nlohmann::json;

[[noreturn]] void ThrowValidationError(std::string_view file, std::string_view path,
                                       std::string_view message);

const Json &RequireMember(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key);
const Json &RequireArray(const Json &object, std::string_view file,
                         std::string_view path, std::string_view key);
std::string RequireString(const Json &object, std::string_view file,
                          std::string_view path, std::string_view key);
double RequireNumber(const Json &object, std::string_view file,
                     std::string_view path, std::string_view key);
std::int64_t RequireInteger(const Json &object, std::string_view file,
                            std::string_view path, std::string_view key);
void RequireExactKeys(const Json &object, std::string_view file,
                      std::initializer_list<std::string_view> expected);

} // namespace hs::content
