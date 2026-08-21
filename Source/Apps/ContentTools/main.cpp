#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../Content/content_source_inventory.hpp"

#include <hs/core/windows_command_line.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef HS_CONTENT_EXECUTABLE
#define HS_CONTENT_EXECUTABLE "hs_content.exe"
#endif

#ifndef HS_PROJECT_EXECUTABLE
#define HS_PROJECT_EXECUTABLE "ProjectHS.exe"
#endif

#ifndef HS_COOKED_DIRECTORY
#define HS_COOKED_DIRECTORY "Cooked"
#endif

#ifndef HS_CMAKE_EXECUTABLE
#define HS_CMAKE_EXECUTABLE "cmake"
#endif

#ifndef HS_BINARY_DIRECTORY
#define HS_BINARY_DIRECTORY "."
#endif

#define HS_WIDEN_IMPL(value) L##value
#define HS_WIDEN(value) HS_WIDEN_IMPL(value)

namespace
{

using Json = nlohmann::ordered_json;

Json ParseDocument(const std::filesystem::path &path, std::string_view category)
{
    Json document;
    try
    {
        document = Json::parse(hs::content::ReadRequiredText(path));
    }
    catch (const Json::exception &exception)
    {
        throw std::runtime_error(path.string() + ": " + exception.what());
    }
    if (!document.is_object() || !document.contains("schema_version") ||
        !document["schema_version"].is_number_integer() ||
        document["schema_version"].get<std::int64_t>() != 1 ||
        !document.contains("category") || !document["category"].is_string() ||
        document["category"].get<std::string>() != category)
    {
        throw std::runtime_error(path.string() +
                                 ": schema_version/category contract mismatch");
    }
    return document;
}

DWORD RunProcess(const std::filesystem::path &executable,
                 std::span<const std::wstring_view> arguments)
{
    std::wstring command =
        hs::QuoteWindowsCommandLineArgument(executable.wstring());
    for (const auto argument : arguments)
    {
        command.push_back(L' ');
        command += hs::QuoteWindowsCommandLineArgument(argument);
    }

    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process))
    {
        throw std::runtime_error(executable.string() +
                                 ": CreateProcess failed with Win32 error " +
                                 std::to_string(GetLastError()));
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    const auto read_exit = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !read_exit)
    {
        throw std::runtime_error(executable.string() + ": process wait failed");
    }
    return exit_code;
}

DWORD RunContentCooker(std::wstring_view mode)
{
    const std::array arguments{mode};
    return RunProcess(HS_CONTENT_EXECUTABLE, arguments);
}

std::uint64_t ReadCookedSourceHash()
{
    const auto manifest = hs::content::ReadRequiredText(
        std::filesystem::path(HS_COOKED_DIRECTORY) / "manifest.txt");
    constexpr std::string_view prefix = "source_hash=";
    const auto start = manifest.find(prefix);
    if (start == std::string::npos)
        throw std::runtime_error("Cook manifest has no source_hash");
    const auto value_start = start + prefix.size();
    const auto value_end = manifest.find_first_of("\r\n", value_start);
    std::uint64_t value{};
    const auto text = std::string_view(manifest).substr(
        value_start, value_end == std::string::npos ? std::string_view::npos
                                                    : value_end - value_start);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Cook manifest source_hash is invalid");
    return value;
}

void PrintCategoryEntryIds(const Json &document, std::string_view category)
{
    if (document.contains("entries") && document["entries"].is_array())
    {
        for (const auto &entry : document["entries"])
        {
            if (entry.is_object() && entry.contains("id") && entry["id"].is_string())
            {
                std::cout << entry["id"].get<std::string>() << '\n';
            }
        }
        return;
    }
    if (category == "upgrades")
    {
        for (const auto &group : document["groups"])
        {
            const auto skill = group["skill_id"].get<std::string>();
            for (const auto &entry : group["entries"])
            {
                std::cout << skill << '#' << entry["ordinal"].get<std::int64_t>() << ' '
                          << entry["logic_id"].get<std::string>() << '\n';
            }
        }
        return;
    }
    if (category == "spawn_schedule")
    {
        constexpr std::array sections = {"continuous_intervals", "compositions", "waves"};
        constexpr std::array prefixes = {"continuous_interval", "composition", "wave"};
        for (std::size_t section = 0; section < sections.size(); ++section)
        {
            for (std::size_t index = 0; index < document[sections[section]].size(); ++index)
            {
                std::cout << prefixes[section] << '#' << index + 1 << '\n';
            }
        }
    }
}

void FindObjectsWithId(Json &value, std::string_view id, std::vector<Json *> &matches)
{
    if (value.is_object())
    {
        const auto iterator = value.find("id");
        if (iterator != value.end() && iterator->is_string() &&
            iterator->get<std::string>() == id)
        {
            matches.push_back(&value);
        }
        for (auto &[unused, child] : value.items())
        {
            static_cast<void>(unused);
            FindObjectsWithId(child, id, matches);
        }
    }
    else if (value.is_array())
    {
        for (auto &child : value)
        {
            FindObjectsWithId(child, id, matches);
        }
    }
}

std::size_t ParseOneBasedIndex(std::string_view value, std::string_view prefix,
                               std::size_t size)
{
    if (!value.starts_with(prefix) || value.size() <= prefix.size() + 1 ||
        value[prefix.size()] != '#')
    {
        throw std::runtime_error("entry ID must use " + std::string(prefix) + "#N");
    }
    std::size_t one_based{};
    const auto digits = value.substr(prefix.size() + 1);
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), one_based);
    if (error != std::errc{} || end != digits.data() + digits.size() || one_based == 0 ||
        one_based > size)
    {
        throw std::runtime_error("entry index is outside category range");
    }
    return one_based - 1;
}

Json &LocateCategoryEntry(Json &document, std::string_view category, std::string_view entry_id)
{
    std::vector<Json *> matches;
    FindObjectsWithId(document, entry_id, matches);
    if (matches.size() == 1)
    {
        return *matches.front();
    }
    if (matches.size() > 1)
    {
        throw std::runtime_error("entry ID is ambiguous");
    }

    if (category == "upgrades")
    {
        const auto separator = entry_id.rfind('#');
        if (separator == std::string_view::npos)
        {
            throw std::runtime_error("upgrade entry ID must use skill.id#ordinal");
        }
        const auto skill = entry_id.substr(0, separator);
        for (auto &group : document["groups"])
        {
            if (group["skill_id"].get<std::string>() != skill)
            {
                continue;
            }
            const auto index = ParseOneBasedIndex(entry_id.substr(separator), "", group["entries"].size());
            return group["entries"][index];
        }
        throw std::runtime_error("unknown upgrade skill ID");
    }

    if (category == "spawn_schedule")
    {
        constexpr std::array sections = {"continuous_intervals", "compositions", "waves"};
        constexpr std::array prefixes = {"continuous_interval", "composition", "wave"};
        for (std::size_t index = 0; index < sections.size(); ++index)
        {
            if (entry_id.starts_with(prefixes[index]))
            {
                const auto entry_index = ParseOneBasedIndex(
                    entry_id, prefixes[index], document[sections[index]].size());
                return document[sections[index]][entry_index];
            }
        }
    }
    throw std::runtime_error("unknown entry ID '" + std::string(entry_id) + "'");
}

void FindNumericParameters(Json &value, std::string_view key, std::vector<Json *> &matches)
{
    if (value.is_object())
    {
        const auto direct = value.find(key);
        if (direct != value.end() && direct->is_number())
        {
            matches.push_back(&*direct);
        }
        const auto parameters = value.find("parameters");
        if (parameters != value.end() && parameters->is_array())
        {
            for (auto &parameter : *parameters)
            {
                if (parameter.is_object() && parameter.contains("key") &&
                    parameter["key"].is_string() &&
                    parameter["key"].get<std::string>() == key &&
                    parameter.contains("value") && parameter["value"].is_number())
                {
                    matches.push_back(&parameter["value"]);
                }
            }
        }
        for (auto &[child_key, child] : value.items())
        {
            if (child_key != key && child_key != "parameters")
            {
                FindNumericParameters(child, key, matches);
            }
        }
    }
    else if (value.is_array())
    {
        for (auto &child : value)
        {
            FindNumericParameters(child, key, matches);
        }
    }
}

Json &LocateNumericParameter(Json &entry, std::string_view key)
{
    std::vector<Json *> matches;
    FindNumericParameters(entry, key, matches);
    if (matches.empty())
    {
        throw std::runtime_error("unknown numeric parameter '" + std::string(key) + "'");
    }
    if (matches.size() != 1)
    {
        throw std::runtime_error("numeric parameter '" + std::string(key) +
                                 "' is ambiguous; use a narrower entry ID");
    }
    return *matches.front();
}

double ParseNumber(std::string_view text)
{
    double value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value,
                        std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() ||
        !std::isfinite(value))
    {
        throw std::runtime_error("value must be one finite number");
    }
    return value;
}

void AssignJsonNumberPreservingType(Json &target, double value)
{
    if (target.is_number_unsigned())
    {
        if (value < 0.0 || std::floor(value) != value ||
            value > static_cast<double>((std::numeric_limits<std::uint64_t>::max)()))
        {
            throw std::runtime_error("value must fit existing unsigned integer type");
        }
        target = static_cast<std::uint64_t>(value);
    }
    else if (target.is_number_integer())
    {
        if (std::floor(value) != value ||
            value < static_cast<double>((std::numeric_limits<std::int64_t>::min)()) ||
            value > static_cast<double>((std::numeric_limits<std::int64_t>::max)()))
        {
            throw std::runtime_error("value must fit existing integer type");
        }
        target = static_cast<std::int64_t>(value);
    }
    else if (target.is_number_float())
    {
        target = value;
    }
    else
    {
        throw std::runtime_error("existing parameter is not numeric");
    }
}

void WriteAndFlush(const std::filesystem::path &path, std::string_view text)
{
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.write(text.data(), static_cast<std::streamsize>(text.size())))
        {
            throw std::runtime_error(path.string() + ": write failed");
        }
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error(path.string() + ": flush open failed");
    }
    const auto flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed)
    {
        throw std::runtime_error(path.string() + ": FlushFileBuffers failed");
    }
}

void SetGameDataParameter(std::string_view category, std::string_view entry_id,
                  std::string_view parameter_key, std::string_view number_text)
{
    const auto path = hs::content::GameDataCategoryPath(category);
    auto document = ParseDocument(path, category);
    auto &entry = LocateCategoryEntry(document, category, entry_id);
    auto &parameter = LocateNumericParameter(entry, parameter_key);
    AssignJsonNumberPreservingType(parameter, ParseNumber(number_text));

    auto temporary = path;
    temporary += L".tmp";
    auto rollback = path;
    rollback += L".rollback";
    DeleteFileW(temporary.c_str());
    DeleteFileW(rollback.c_str());
    WriteAndFlush(temporary, document.dump(2, ' ', false, Json::error_handler_t::strict) + "\n");

    if (!ReplaceFileW(path.c_str(), temporary.c_str(), rollback.c_str(),
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
    {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error(path.string() +
                                 ": atomic replace failed with Win32 error " +
                                 std::to_string(GetLastError()));
    }

    DWORD validation_exit{};
    try
    {
        validation_exit = RunContentCooker(L"--validate-only");
    }
    catch (...)
    {
        if (!ReplaceFileW(path.c_str(), rollback.c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
        {
            throw std::runtime_error(
                path.string() +
                ": validator launch failed and rollback failed with Win32 error " +
                std::to_string(GetLastError()));
        }
        throw;
    }
    if (validation_exit == 0)
    {
        DeleteFileW(rollback.c_str());
        std::cout << "tools.set applied category=" << category << " entry=" << entry_id
                  << " parameter=" << parameter_key << '\n';
        return;
    }

    if (!ReplaceFileW(path.c_str(), rollback.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
                      nullptr, nullptr))
    {
        throw std::runtime_error(path.string() +
                                 ": validation failed and rollback failed with Win32 error " +
                                 std::to_string(GetLastError()));
    }
    throw std::runtime_error("hs_content validation failed; source restored");
}

void PrintUsage()
{
    std::cerr << "usage:\n"
              << "  hs_content_tools validate\n"
              << "  hs_content_tools cook\n"
              << "  hs_content_tools status\n"
              << "  hs_content_tools hot-reload\n"
              << "  hs_content_tools shader-reload\n"
              << "  hs_content_tools list <category>\n"
              << "  hs_content_tools set <category> <entry-id> <parameter-key> <number>\n"
              << "  hs_content_tools preview\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 2;
    }

    try
    {
        const std::string_view command = argv[1];
        if (command == "validate" && argc == 2)
        {
            const auto exit_code = RunContentCooker(L"--validate-only");
            if (exit_code != 0)
            {
                std::cerr << "tools.error hs_content validation exit=" << exit_code << '\n';
                return 1;
            }
            std::cout << "tools.validated documents=" << hs::content::kDocumentNames.size()
                      << '\n';
            return 0;
        }
        if (command == "cook" && argc == 2)
        {
            const auto exit_code = RunContentCooker(L"--cook");
            if (exit_code != 0)
            {
                std::cerr << "tools.error hs_content Cook exit=" << exit_code << '\n';
                return 1;
            }
            std::cout << "tools.cooked\n";
            return 0;
        }
        if (command == "status" && argc == 2)
        {
            const auto source = hs::content::ComputeContentSourceHash(
                hs::content::LoadContentSourceInventory());
            const auto cooked = ReadCookedSourceHash();
            std::cout << "tools.status source_hash=" << source
                      << " cooked_hash=" << cooked
                      << " synchronized=" << (source == cooked ? "true" : "false")
                      << '\n';
            return source == cooked ? 0 : 1;
        }
        if (command == "hot-reload" && argc == 2)
        {
            const auto exit_code = RunContentCooker(L"--cook");
            if (exit_code != 0)
                return 1;
            std::cout << "tools.hot_reload requested; running game applies it only at main menu\n";
            return 0;
        }
        if (command == "shader-reload" && argc == 2)
        {
            constexpr std::array<std::wstring_view, 4> arguments{
                L"--build", HS_WIDEN(HS_BINARY_DIRECTORY), L"--target",
                L"hs_runtime_assets"};
            const auto exit_code = RunProcess(HS_CMAKE_EXECUTABLE, arguments);
            if (exit_code != 0)
                return 1;
            std::cout << "tools.shader_reload requested; running Debug game swaps valid PSOs\n";
            return 0;
        }
        if (command == "list" && argc == 3)
        {
            const std::string_view category = argv[2];
            PrintCategoryEntryIds(
                ParseDocument(hs::content::GameDataCategoryPath(category), category), category);
            return 0;
        }
        if (command == "set" && argc == 6)
        {
            SetGameDataParameter(argv[2], argv[3], argv[4], argv[5]);
            return 0;
        }
        if (command == "preview" && argc == 2)
        {
            std::cout << "tools.preview authored archer with procedural combat placeholders\n";
            const std::span<const std::wstring_view> no_arguments;
            return RunProcess(HS_PROJECT_EXECUTABLE, no_arguments) == 0 ? 0 : 1;
        }
        PrintUsage();
        return 2;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "tools.error " << exception.what() << '\n';
        return 1;
    }
}
