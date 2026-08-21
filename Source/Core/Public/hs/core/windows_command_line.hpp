#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace hs
{

// Quotes one argument using the CommandLineToArgvW/MSVC parsing rules.
[[nodiscard]] inline std::wstring
QuoteWindowsCommandLineArgument(std::wstring_view value)
{
    std::wstring quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back(L'"');

    std::size_t backslashes{};
    for (const auto character : value)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        }
        else
        {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }

    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace hs
