#include <hs/runtime/save_store.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace hs
{
namespace
{

using Json = nlohmann::json;

constexpr std::uint64_t kMaximumSaveBytes = 1024 * 1024;
constexpr std::wstring_view kSettingsName = L"settings.json";
constexpr std::wstring_view kProfileName = L"profile.json";

enum class ReadStatus
{
    Missing,
    Valid,
    Corrupt,
};

class UniqueHandle
{
  public:
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;

    [[nodiscard]] HANDLE Get() const noexcept
    {
        return handle_;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] Result Win32Failure(std::string_view operation, DWORD error = GetLastError())
{
    return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                           std::format("{} failed with Win32 error {}.", operation, error));
}

[[nodiscard]] std::filesystem::path BackupPath(const std::filesystem::path &path)
{
    auto backup = path;
    backup += L".bak";
    return backup;
}

[[nodiscard]] std::filesystem::path TemporaryPath(const std::filesystem::path &path)
{
    auto temporary = path;
    temporary += L".tmp";
    return temporary;
}

[[nodiscard]] Result ReadText(const std::filesystem::path &path, std::string &text,
                              bool &missing)
{
    missing = false;
    const auto raw = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            missing = true;
            return Result::Success();
        }
        return Win32Failure("CreateFileW(read save)", error);
    }
    UniqueHandle file(raw);

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size))
    {
        return Win32Failure("GetFileSizeEx(save)");
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > kMaximumSaveBytes)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                               "Save file exceeds the size limit.");
    }

    text.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset{};
    while (offset < text.size())
    {
        const auto remaining = std::min<std::size_t>(
            text.size() - offset, std::numeric_limits<DWORD>::max());
        DWORD read{};
        if (!ReadFile(file.Get(), text.data() + offset, static_cast<DWORD>(remaining), &read,
                      nullptr))
        {
            return Win32Failure("ReadFile(save)");
        }
        if (read == 0)
        {
            text.clear();
            return Result::Success();
        }
        offset += read;
    }
    return Result::Success();
}

[[nodiscard]] bool ReadUnsigned(const Json &json, std::string_view key,
                                std::uint64_t &value)
{
    const auto iterator = json.find(key);
    if (iterator == json.end() || !iterator->is_number_unsigned())
    {
        return false;
    }
    value = iterator->get<std::uint64_t>();
    return true;
}

template <typename Integer>
[[nodiscard]] bool ReadUnsigned(const Json &json, std::string_view key, Integer &value)
{
    std::uint64_t wide{};
    if (!ReadUnsigned(json, key, wide) || wide > std::numeric_limits<Integer>::max())
    {
        return false;
    }
    value = static_cast<Integer>(wide);
    return true;
}

[[nodiscard]] bool ReadVolume(const Json &json, std::string_view key, float &value)
{
    const auto iterator = json.find(key);
    if (iterator == json.end() || !iterator->is_number())
    {
        return false;
    }
    const auto number = iterator->get<double>();
    if (!std::isfinite(number) || number < 0.0 || number > 1.0)
    {
        return false;
    }
    value = static_cast<float>(number);
    return true;
}

[[nodiscard]] bool ValidSkillKeys(const std::array<std::uint16_t, 4> &keys) noexcept
{
    for (std::size_t index = 0; index < keys.size(); ++index)
    {
        if (keys[index] == 0 || keys[index] > 0xFE ||
            std::find(keys.begin(), keys.begin() + index, keys[index]) !=
                keys.begin() + index)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ValidVolume(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

[[nodiscard]] bool ReadSkillKeys(const Json &json,
                                 std::array<std::uint16_t, 4> &keys)
{
    const auto iterator = json.find("skill_virtual_keys");
    if (iterator == json.end() || !iterator->is_array() ||
        iterator->size() != keys.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < keys.size(); ++index)
    {
        const auto &value = (*iterator)[index];
        if (!value.is_number_unsigned() ||
            value.get<std::uint64_t>() > std::numeric_limits<std::uint16_t>::max())
        {
            return false;
        }
        keys[index] = static_cast<std::uint16_t>(value.get<std::uint64_t>());
    }
    return ValidSkillKeys(keys);
}

[[nodiscard]] bool ValidProfile(const ProfileData &profile) noexcept
{
    return profile.schema_version == ProfileData::kSchemaVersion && profile.best_level >= 1;
}

template <typename T>
[[nodiscard]] Json EncodeGraphicsValue(const T &value)
{
    if constexpr (std::is_enum_v<T>)
    {
        return GraphicsEnumToken(value);
    }
    else if constexpr (std::is_same_v<T, GraphicsDisplayId> ||
                       std::is_same_v<T, GraphicsProviderId> ||
                       std::is_same_v<T, GraphicsModeId>)
    {
        return value.value;
    }
    else if constexpr (std::is_same_v<T, GraphicsOutputExtent>)
    {
        return value ? Json{{"width", value->width}, {"height", value->height}}
                     : Json("auto");
    }
    else if constexpr (std::is_same_v<T, GraphicsAutoFloat>)
    {
        return value ? Json(*value) : Json("auto");
    }
    else
    {
        return value;
    }
}

template <typename T>
[[nodiscard]] bool DecodeGraphicsValue(const Json &json, T &value)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        if (!json.is_boolean()) return false;
        value = json.get<bool>();
    }
    else if constexpr (std::is_same_v<T, std::uint32_t>)
    {
        if (!json.is_number_unsigned() ||
            json.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
            return false;
        value = static_cast<std::uint32_t>(json.get<std::uint64_t>());
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        if (!json.is_number() || !std::isfinite(json.get<double>())) return false;
        value = json.get<float>();
    }
    else if constexpr (std::is_enum_v<T>)
    {
        if (!json.is_string()) return false;
        const auto token = json.get<std::string>();
        const auto parsed = ParseGraphicsEnumToken<T>(token);
        if (!parsed) return false;
        value = *parsed;
    }
    else if constexpr (std::is_same_v<T, GraphicsDisplayId> ||
                       std::is_same_v<T, GraphicsProviderId> ||
                       std::is_same_v<T, GraphicsModeId>)
    {
        if (!json.is_string()) return false;
        value.value = json.get<std::string>();
    }
    else if constexpr (std::is_same_v<T, GraphicsOutputExtent>)
    {
        if (json.is_string() && json.get<std::string>() == "auto")
        {
            value.reset();
        }
        else if (json.is_object() && json.size() == 2)
        {
            GraphicsExtent extent;
            if (!ReadUnsigned(json, "width", extent.width) ||
                !ReadUnsigned(json, "height", extent.height)) return false;
            value = extent;
        }
        else return false;
    }
    else if constexpr (std::is_same_v<T, GraphicsAutoFloat>)
    {
        if (json.is_string() && json.get<std::string>() == "auto")
        {
            value.reset();
        }
        else if (json.is_number() && std::isfinite(json.get<double>()))
        {
            value = json.get<float>();
        }
        else return false;
    }
    return true;
}

[[nodiscard]] bool KnownSettingsKey(std::string_view key) noexcept
{
    return key == "schema_version" || key == "graphics" ||
           key == "master_volume" || key == "bgm_volume" ||
           key == "sfx_volume" || key == "ui_volume" ||
           key == "skill_virtual_keys" || key == "screen_overlay_intensity";
}

[[nodiscard]] bool RemovedGraphicsSettingKey(std::string_view key) noexcept
{
    return key == "ray_tracing_enabled" || key == "rt_trace_distance_m" ||
           key == "neural_ray_reconstruction" || key == "ray_reconstruction_provider" ||
           key == "gi_quality" || key == "lighting_quality" || key == "outline" || key == "outline_width_px";
}

[[nodiscard]] bool ParseExtensions(std::string_view text, Json &extensions)
{
    if (text.size() > kMaximumSaveBytes) return false;
    extensions = Json::parse(text, nullptr, false);
    if (extensions.is_discarded() || !extensions.is_object()) return false;
    for (auto iterator = extensions.begin(); iterator != extensions.end(); ++iterator)
    {
        if ((iterator.key() != "top" && iterator.key() != "graphics") ||
            !iterator->is_object()) return false;
        for (auto field = iterator->begin(); field != iterator->end(); ++field)
        {
            if ((iterator.key() == "top" && KnownSettingsKey(field.key())) ||
                (iterator.key() == "graphics" &&
                 (ParseGraphicsSettingKey(field.key()) || RemovedGraphicsSettingKey(field.key())))) return false;
        }
    }
    return true;
}

[[nodiscard]] bool ValidSettingsValues(const SettingsDocument &settings)
{
    const auto extent = settings.graphics.output_extent;
    const bool valid_extent = !extent ||
        (extent->width >= 640 && extent->width <= 16'384 &&
         extent->height >= 360 && extent->height <= 8'640);
    return settings.schema_version == SettingsDocument::kSchemaVersion &&
           valid_extent &&
           ValidVolume(settings.master_volume) && ValidVolume(settings.bgm_volume) &&
           ValidVolume(settings.sfx_volume) && ValidVolume(settings.ui_volume) &&
           ValidVolume(settings.screen_overlay_intensity) &&
           ValidSkillKeys(settings.skill_virtual_keys) &&
           ValidateGraphicsSettings(settings.graphics).empty();
}

[[nodiscard]] Json ToJson(const SettingsDocument &settings, const Json &extensions)
{
    Json graphics = Json::object();
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) \
    graphics[#field] = EncodeGraphicsValue(settings.graphics.field);
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
    Json document{{"schema_version", settings.schema_version},
                  {"graphics", std::move(graphics)},
                  {"master_volume", settings.master_volume},
                  {"bgm_volume", settings.bgm_volume},
                  {"sfx_volume", settings.sfx_volume},
                  {"ui_volume", settings.ui_volume},
                  {"skill_virtual_keys", settings.skill_virtual_keys},
                  {"screen_overlay_intensity", settings.screen_overlay_intensity}};
    for (const auto &scope : {"top", "graphics"})
    {
        const auto iterator = extensions.find(scope);
        if (iterator == extensions.end()) continue;
        auto &target = scope == std::string_view("top") ? document : document["graphics"];
        for (auto field = iterator->begin(); field != iterator->end(); ++field)
            target[field.key()] = field.value();
    }
    return document;
}

[[nodiscard]] Json ToJson(const SettingsDocument &settings)
{
    // Recovery writes only documents already admitted by ReadDocument.
    return ToJson(settings, Json::parse(settings.opaque_optional_json));
}

[[nodiscard]] Json ToJson(const ProfileData &profile)
{
    return {{"schema_version", profile.schema_version},
            {"best_level", profile.best_level},
            {"total_wins", profile.total_wins},
            {"total_kills", profile.total_kills}};
}

[[nodiscard]] bool FromJson(const Json &json, SettingsDocument &settings)
{
    if (!json.is_object()) return false;
    SettingsDocument parsed;
    const auto graphics = json.find("graphics");
    if (!ReadUnsigned(json, "schema_version", parsed.schema_version) ||
        parsed.schema_version != SettingsDocument::kSchemaVersion ||
        graphics == json.end() || !graphics->is_object() ||
        !ReadVolume(json, "master_volume", parsed.master_volume) ||
        !ReadVolume(json, "bgm_volume", parsed.bgm_volume) ||
        !ReadVolume(json, "sfx_volume", parsed.sfx_volume) ||
        !ReadVolume(json, "ui_volume", parsed.ui_volume) ||
        !ReadSkillKeys(json, parsed.skill_virtual_keys)) return false;
    if (json.contains("screen_overlay_intensity") &&
        !ReadVolume(json, "screen_overlay_intensity", parsed.screen_overlay_intensity))
        return false;

#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) \
    if (const auto value = graphics->find(#field); value != graphics->end() && \
        !DecodeGraphicsValue(*value, parsed.graphics.field)) return false;
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING

    Json extensions = Json::object();
    for (auto iterator = json.begin(); iterator != json.end(); ++iterator)
    {
        if (!KnownSettingsKey(iterator.key()))
            extensions["top"][iterator.key()] = iterator.value();
    }
    for (auto iterator = graphics->begin(); iterator != graphics->end(); ++iterator)
    {
        if (!ParseGraphicsSettingKey(iterator.key()) && !RemovedGraphicsSettingKey(iterator.key()))
            extensions["graphics"][iterator.key()] = iterator.value();
    }
    parsed.opaque_optional_json = extensions.dump();
    if (!ValidSettingsValues(parsed) || parsed.opaque_optional_json.size() > kMaximumSaveBytes)
        return false;
    settings = std::move(parsed);
    return true;
}

[[nodiscard]] bool FromJson(const Json &json, ProfileData &profile)
{
    if (!json.is_object() || json.size() != 4u) return false;
    ProfileData parsed;
    if (!ReadUnsigned(json, "schema_version", parsed.schema_version) ||
        !ReadUnsigned(json, "best_level", parsed.best_level) ||
        !ReadUnsigned(json, "total_wins", parsed.total_wins) ||
        !ReadUnsigned(json, "total_kills", parsed.total_kills) ||
        !ValidProfile(parsed))
    {
        return false;
    }
    profile = parsed;
    return true;
}

template <typename Data>
[[nodiscard]] Result ReadDocument(const std::filesystem::path &path, Data &data,
                                  ReadStatus &status)
{
    std::string text;
    bool missing{};
    if (auto read = ReadText(path, text, missing); !read)
    {
        return read;
    }
    if (missing)
    {
        status = ReadStatus::Missing;
        return Result::Success();
    }

    try
    {
        Json json = Json::parse(text, nullptr, false);
        if (json.is_discarded())
        {
            status = ReadStatus::Corrupt;
            return Result::Success();
        }
        std::uint64_t schema{};
        if (json.is_object() && ReadUnsigned(json, "schema_version", schema))
        {
            if (schema != Data::kSchemaVersion)
                return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                       "Unsupported save schema.");
        }
        if (FromJson(json, data))
        {
            status = ReadStatus::Valid;
            return Result::Success();
        }
    }
    catch (const Json::exception &)
    {
    }
    status = ReadStatus::Corrupt;
    return Result::Success();
}

[[nodiscard]] Result EnsureRoot(const std::filesystem::path &root)
{
    if (root.empty())
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "LOCALAPPDATA is unavailable and no save root was provided.");
    }
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("Creating save directory failed with error {}.",
                                           error.value()));
    }
    return Result::Success();
}

[[nodiscard]] Result WriteText(const std::filesystem::path &target, std::string_view text)
{
    if (auto root = EnsureRoot(target.parent_path()); !root)
    {
        return root;
    }

    const auto temporary = TemporaryPath(target);
    {
        const auto raw = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE)
        {
            return Win32Failure("CreateFileW(write save)");
        }
        UniqueHandle file(raw);

        std::size_t offset{};
        while (offset < text.size())
        {
            const auto remaining = std::min<std::size_t>(
                text.size() - offset, std::numeric_limits<DWORD>::max());
            DWORD written{};
            if (!WriteFile(file.Get(), text.data() + offset,
                           static_cast<DWORD>(remaining), &written, nullptr) || written == 0)
            {
                return Win32Failure("WriteFile(save)");
            }
            offset += written;
        }
        if (!FlushFileBuffers(file.Get()))
        {
            return Win32Failure("FlushFileBuffers(save)");
        }
    }

    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        const auto backup = BackupPath(target);
        if (!ReplaceFileW(target.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr,
                          nullptr))
        {
            return Win32Failure("ReplaceFileW(save)");
        }
        return Result::Success();
    }

    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    {
        return Win32Failure("GetFileAttributesW(save)", error);
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        return Win32Failure("MoveFileExW(first save)");
    }
    return Result::Success();
}

[[nodiscard]] Result WriteDocument(const std::filesystem::path &path, const Json &document)
{
    try
    {
        auto text = document.dump(2);
        text.push_back('\n');
        if (text.size() > kMaximumSaveBytes)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                                   "Serialized save exceeds the size limit.");
        return WriteText(path, text);
    }
    catch (const Json::exception &exception)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               std::format("Serializing save failed: {}", exception.what()));
    }
}

[[nodiscard]] std::wstring CorruptTimestamp()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return std::format(L"{:04}{:02}{:02}T{:02}{:02}{:02}.{:03}Z.{}", time.wYear,
                       time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                       time.wMilliseconds, GetCurrentProcessId());
}

[[nodiscard]] Result PreserveCorrupt(const std::filesystem::path &path)
{
    const auto timestamp = CorruptTimestamp();
    for (unsigned suffix = 0; suffix < 100; ++suffix)
    {
        auto name = path.filename().wstring() + L"." + timestamp;
        if (suffix != 0)
        {
            name += std::format(L".{}", suffix);
        }
        name += L".corrupt";
        const auto destination = path.parent_path() / name;
        if (MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            return Result::Success();
        }
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
        {
            return Win32Failure("MoveFileExW(preserve corrupt save)", error);
        }
    }
    return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                           "Could not allocate a unique corrupt save name.");
}

template <typename Data>
[[nodiscard]] Result Load(const std::filesystem::path &path, Data &data,
                          bool *loaded_from_file = nullptr)
{
    data = Data{};
    if (loaded_from_file) *loaded_from_file = false;
    if (path.parent_path().empty())
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "LOCALAPPDATA is unavailable and no save root was provided.");
    }
    Data primary;
    ReadStatus primary_status{};
    if (auto read = ReadDocument(path, primary, primary_status); !read)
    {
        return read;
    }
    if (primary_status == ReadStatus::Valid)
    {
        data = primary;
        if (loaded_from_file) *loaded_from_file = true;
        return Result::Success();
    }

    const auto backup_path = BackupPath(path);
    Data backup;
    ReadStatus backup_status{};
    if (auto read = ReadDocument(backup_path, backup, backup_status); !read)
    {
        return read;
    }
    if (backup_status == ReadStatus::Valid)
    {
        if (primary_status == ReadStatus::Corrupt)
        {
            if (auto preserved = PreserveCorrupt(path); !preserved)
            {
                return preserved;
            }
        }
        if (auto restored = WriteDocument(path, ToJson(backup)); !restored)
        {
            return restored;
        }
        data = backup;
        if (loaded_from_file) *loaded_from_file = true;
        return Result::Success();
    }

    const auto had_corruption = primary_status == ReadStatus::Corrupt ||
                                backup_status == ReadStatus::Corrupt;
    if (primary_status == ReadStatus::Corrupt)
    {
        if (auto preserved = PreserveCorrupt(path); !preserved)
        {
            return preserved;
        }
    }
    if (backup_status == ReadStatus::Corrupt)
    {
        if (auto preserved = PreserveCorrupt(backup_path); !preserved)
        {
            return preserved;
        }
    }
    if (had_corruption)
    {
        // Without a valid saved document, Runtime must continue to distinguish
        // first-run/legacy effective defaults from an explicitly saved v2 default.
        // The corrupt copies are already preserved above; leave the primary absent.
        if constexpr (std::is_same_v<Data, SettingsDocument>)
            return Result::Success();
        return WriteDocument(path, ToJson(data));
    }
    return Result::Success();
}

template <typename Data>
[[nodiscard]] Result Save(const std::filesystem::path &path, const Data &data)
{
    Json selected;
    if constexpr (std::is_same_v<Data, SettingsDocument>)
    {
        if (!ValidSettingsValues(data) || !ParseExtensions(data.opaque_optional_json, selected))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                                   "SettingsDocument is invalid.");
    }
    else if (!ValidProfile(data))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime", "ProfileData is invalid.");
    if (path.parent_path().empty())
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "LOCALAPPDATA is unavailable and no save root was provided.");
    }

    Data existing;
    ReadStatus status{};
    if (auto read = ReadDocument(path, existing, status); !read)
    {
        return read;
    }
    Data existing_backup;
    ReadStatus backup_status{};
    if (auto read = ReadDocument(BackupPath(path), existing_backup, backup_status); !read)
    {
        return read;
    }
    if constexpr (std::is_same_v<Data, SettingsDocument>)
    {
        const auto retain = [&](const Data &old) {
            const auto previous = Json::parse(old.opaque_optional_json);
            for (const auto &scope : {"top", "graphics"})
            {
                const auto previous_scope = previous.find(scope);
                if (previous_scope == previous.end()) continue;
                for (auto field = previous_scope->begin(); field != previous_scope->end();
                     ++field)
                {
                    if (!selected[scope].contains(field.key()))
                        selected[scope][field.key()] = field.value();
                }
            }
        };
        if (status == ReadStatus::Valid) retain(existing);
        if (backup_status == ReadStatus::Valid) retain(existing_backup);
        if (selected.dump().size() > kMaximumSaveBytes)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                                   "Merged settings document is invalid.");
    }
    if (status == ReadStatus::Corrupt)
    {
        if (auto preserved = PreserveCorrupt(path); !preserved)
        {
            return preserved;
        }
    }
    if constexpr (std::is_same_v<Data, SettingsDocument>)
        return WriteDocument(path, ToJson(data, selected));
    else
        return WriteDocument(path, ToJson(data));
}

} // namespace

std::filesystem::path SaveStore::DefaultRoot()
{
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0)
    {
        return {};
    }

    std::wstring value(size, L'\0');
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    if (length == 0 || length >= size)
    {
        return {};
    }
    value.resize(length);
    return std::filesystem::path(std::move(value)) / L"ProjectHS";
}

SaveStore::SaveStore(std::optional<std::filesystem::path> root_override)
    : root_(root_override ? std::move(*root_override) : SaveStore::DefaultRoot())
{
}

Result SaveStore::LoadSettingsV2(SettingsDocument &settings,
                                 bool *loaded_from_file) const
{
    return Load(root_ / kSettingsName, settings, loaded_from_file);
}

Result SaveStore::SaveSettingsV2(const SettingsDocument &settings) const
{
    return Save(root_ / kSettingsName, settings);
}

Result SaveStore::LoadProfile(ProfileData &profile) const
{
    return Load(root_ / kProfileName, profile);
}

Result SaveStore::SaveProfile(const ProfileData &profile) const
{
    return Save(root_ / kProfileName, profile);
}

} // namespace hs
