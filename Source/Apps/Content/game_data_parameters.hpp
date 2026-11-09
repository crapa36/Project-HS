#pragma once

#include "content_cooker.hpp"

#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hs::content
{

class ParameterValues
{
public:
    // Key/value storage belongs to the source entry throughout this cook.
    ParameterValues(const Json &entry, std::string_view file, std::string_view path);
    ParameterValues(const Json &&, std::string_view, std::string_view) = delete;
    [[nodiscard]] std::span<const std::pair<std::string_view, const Json *>> Values() const noexcept
    {
        return values_;
    }

private:
    std::vector<std::pair<std::string_view, const Json *>> values_;
};

const Json &FindParameterValue(const ParameterValues &parameters, std::string_view file,
                               std::string_view path, std::string_view key);
double ParameterNumber(const ParameterValues &parameters, std::string_view file, std::string_view path,
                       std::string_view key);

template <typename Expected>
void RequireParameterKeys(const ParameterValues &parameters, std::string_view file,
                          std::string_view path, const Expected &expected)
{
    std::set<std::string, std::less<>> keys;
    for (const auto key : expected) keys.emplace(key);
    for (const auto &[key, unused] : parameters.Values())
    {
        static_cast<void>(unused);
        const auto expected_key = keys.find(key);
        if (expected_key == keys.end())
            ThrowValidationError(file, std::string(path) + "/parameters/" +
                std::string(key),
                "unexpected or duplicate parameter key");
        keys.erase(expected_key);
    }
    if (!keys.empty())
        ThrowValidationError(file, std::string(path) + "/parameters", "missing parameter '" + *keys.begin() + "'");
}

inline void RequireParameterKeys(const ParameterValues &parameters, std::string_view file,
                                 std::string_view path,
                                 std::initializer_list<std::string_view> expected)
{
    RequireParameterKeys<std::initializer_list<std::string_view>>(parameters, file, path, expected);
}

} // namespace hs::content
