#pragma once

#include <hs/asset_manifest/selected_generation.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace test_generation
{

inline std::optional<std::wstring> Environment(const wchar_t *name)
{
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
        return std::nullopt;
    std::wstring value(size, L'\0');
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size)
        throw std::runtime_error("cannot read test Generation environment");
    value.resize(length);
    return value;
}

inline std::filesystem::path ExecutableDirectory()
{
    std::wstring name(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(nullptr, name.data(),
                                                static_cast<DWORD>(name.size()));
        if (length == 0)
            throw std::runtime_error("cannot locate test executable");
        if (length < name.size())
        {
            name.resize(length);
            return std::filesystem::path(name).parent_path();
        }
        if (name.size() >= 32768)
            throw std::runtime_error("test executable path exceeds Windows limit");
        name.resize(name.size() * 2);
    }
}

inline const hs::SelectedGeneration &Selected()
{
    static const auto generation = [] {
        hs::SelectedGeneration loaded;
        const auto configured_pointer = Environment(L"HS_TEST_GENERATION_POINTER");
        const auto configured_scope = Environment(L"HS_TEST_GENERATION_SCOPE");
        if (configured_pointer.has_value() != configured_scope.has_value())
            throw std::runtime_error("test Generation pointer and scope must be configured together");
        const auto pointer = configured_pointer
            ? std::filesystem::path(*configured_pointer)
            : ExecutableDirectory() / "GenerationState" / "runtime-full.json";
        if (!pointer.is_absolute())
            throw std::runtime_error("test Generation pointer must be absolute");
        const auto scope = configured_scope
            ? std::string(configured_scope->begin(), configured_scope->end())
            : std::string("runtime-full");
        const auto result = hs::LoadSelectedGeneration(pointer, loaded);
        if (!result.Succeeded())
            throw std::runtime_error("cannot load selected test Generation " +
                                     pointer.string() + ": " +
                                     std::string(result.Message()));
        if (loaded.scope != scope)
            throw std::runtime_error("selected test Generation has wrong scope: " + loaded.scope +
                                     " (expected " + scope + ")");
        return loaded;
    }();
    return generation;
}

inline std::filesystem::path ProductPath(std::string_view logical_id)
{
    std::filesystem::path path;
    const auto result = Selected().Require(logical_id, path);
    if (!result.Succeeded())
        throw std::runtime_error(std::string(result.Message()));
    return path;
}

} // namespace test_generation
