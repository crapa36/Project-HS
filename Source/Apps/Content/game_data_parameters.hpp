#pragma once

#include "game_data_cooker.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
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
    for (const auto &[key, unused] : parameters.Values())
    {
        static_cast<void>(unused);
        if (std::ranges::find(expected, key) == std::ranges::end(expected))
            ThrowValidationError(file, std::string(path) + "/parameters/" +
                std::string(key),
                "unexpected or duplicate parameter key");
    }
    std::optional<std::string_view> missing;
    for (const auto &key : expected)
    {
        const auto view = std::string_view(key);
        if (!std::ranges::binary_search(parameters.Values(), view, {},
                [](const auto &row) { return row.first; }) && (!missing || view < *missing))
            missing = view;
    }
    if (missing)
        ThrowValidationError(file, std::string(path) + "/parameters", "missing parameter '" +
            std::string(*missing) + "'");
}

inline void RequireParameterKeys(const ParameterValues &parameters, std::string_view file,
                                 std::string_view path,
                                 std::initializer_list<std::string_view> expected)
{
    RequireParameterKeys<std::initializer_list<std::string_view>>(parameters, file, path, expected);
}

} // namespace hs::content
