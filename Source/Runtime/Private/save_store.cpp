#include <hs/runtime/save_store.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
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

[[nodiscard]] std::filesystem::path DefaultRoot()
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
        text.clear();
        return Result::Success();
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

[[nodiscard]] bool ReadBool(const Json &json, std::string_view key, bool &value)
{
    const auto iterator = json.find(key);
    if (iterator == json.end() || !iterator->is_boolean())
    {
        return false;
    }
    value = iterator->get<bool>();
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

[[nodiscard]] bool ValidSettings(const SettingsData &settings) noexcept
{
    if (settings.schema_version != SettingsData::kSchemaVersion || settings.width < 640 ||
        settings.width > 16'384 || settings.height < 360 || settings.height > 8'640 ||
        (settings.frame_cap != 0 && settings.frame_cap != 30 && settings.frame_cap != 60 &&
         settings.frame_cap != 120) ||
        (settings.render_scale_percent != 75 && settings.render_scale_percent != 100) ||
        (settings.shadow_resolution != 1024 && settings.shadow_resolution != 2048) ||
        (settings.particle_percentage != 50 && settings.particle_percentage != 100) ||
        !std::isfinite(settings.master_volume) || settings.master_volume < 0.0f ||
        settings.master_volume > 1.0f || !std::isfinite(settings.bgm_volume) ||
        settings.bgm_volume < 0.0f || settings.bgm_volume > 1.0f ||
        !std::isfinite(settings.sfx_volume) || settings.sfx_volume < 0.0f ||
        settings.sfx_volume > 1.0f || !std::isfinite(settings.ui_volume) ||
        settings.ui_volume < 0.0f || settings.ui_volume > 1.0f)
    {
        return false;
    }

    for (std::size_t index = 0; index < settings.skill_virtual_keys.size(); ++index)
    {
        const auto key = settings.skill_virtual_keys[index];
        if (key == 0 || key > 0xFE ||
            std::find(settings.skill_virtual_keys.begin(),
                      settings.skill_virtual_keys.begin() + index,
                      key) != settings.skill_virtual_keys.begin() + index)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ValidProfile(const ProfileData &profile) noexcept
{
    return profile.schema_version == ProfileData::kSchemaVersion &&
           profile.best_level >= 1 && (profile.unlocked_skills_mask & ~0xFFull) == 0 &&
           (profile.unlocked_relics_mask & ~0xFFFull) == 0;
}

[[nodiscard]] Json ToJson(const SettingsData &settings)
{
    return {{"schema_version", settings.schema_version},
            {"width", settings.width},
            {"height", settings.height},
            {"borderless", settings.borderless},
            {"vsync", settings.vsync},
            {"frame_cap", settings.frame_cap},
            {"render_scale_percent", settings.render_scale_percent},
            {"shadow_resolution", settings.shadow_resolution},
            {"particle_percentage", settings.particle_percentage},
            {"bloom", settings.bloom},
            {"outline", settings.outline},
            {"master_volume", settings.master_volume},
            {"bgm_volume", settings.bgm_volume},
            {"sfx_volume", settings.sfx_volume},
            {"ui_volume", settings.ui_volume},
            {"skill_virtual_keys", settings.skill_virtual_keys}};
}

[[nodiscard]] Json ToJson(const ProfileData &profile)
{
    return {{"schema_version", profile.schema_version},
            {"best_level", profile.best_level},
            {"total_wins", profile.total_wins},
            {"total_kills", profile.total_kills},
            {"unlocked_skills_mask", profile.unlocked_skills_mask},
            {"unlocked_relics_mask", profile.unlocked_relics_mask}};
}

[[nodiscard]] bool FromJson(const Json &json, SettingsData &settings)
{
    if (!json.is_object() || json.size() != 16)
    {
        return false;
    }

    SettingsData parsed;
    const auto keys = json.find("skill_virtual_keys");
    if (!ReadUnsigned(json, "schema_version", parsed.schema_version) ||
        !ReadUnsigned(json, "width", parsed.width) ||
        !ReadUnsigned(json, "height", parsed.height) ||
        !ReadBool(json, "borderless", parsed.borderless) ||
        !ReadBool(json, "vsync", parsed.vsync) ||
        !ReadUnsigned(json, "frame_cap", parsed.frame_cap) ||
        !ReadUnsigned(json, "render_scale_percent", parsed.render_scale_percent) ||
        !ReadUnsigned(json, "shadow_resolution", parsed.shadow_resolution) ||
        !ReadUnsigned(json, "particle_percentage", parsed.particle_percentage) ||
        !ReadBool(json, "bloom", parsed.bloom) ||
        !ReadBool(json, "outline", parsed.outline) ||
        !ReadVolume(json, "master_volume", parsed.master_volume) ||
        !ReadVolume(json, "bgm_volume", parsed.bgm_volume) ||
        !ReadVolume(json, "sfx_volume", parsed.sfx_volume) ||
        !ReadVolume(json, "ui_volume", parsed.ui_volume) || keys == json.end() ||
        !keys->is_array() || keys->size() != parsed.skill_virtual_keys.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < parsed.skill_virtual_keys.size(); ++index)
    {
        const auto &key = (*keys)[index];
        if (!key.is_number_unsigned())
        {
            return false;
        }
        const auto value = key.get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint16_t>::max())
        {
            return false;
        }
        parsed.skill_virtual_keys[index] = static_cast<std::uint16_t>(value);
    }
    if (!ValidSettings(parsed))
    {
        return false;
    }
    settings = parsed;
    return true;
}

[[nodiscard]] bool FromJson(const Json &json, ProfileData &profile)
{
    if (!json.is_object() || json.size() != 6)
    {
        return false;
    }

    ProfileData parsed;
    if (!ReadUnsigned(json, "schema_version", parsed.schema_version) ||
        !ReadUnsigned(json, "best_level", parsed.best_level) ||
        !ReadUnsigned(json, "total_wins", parsed.total_wins) ||
        !ReadUnsigned(json, "total_kills", parsed.total_kills) ||
        !ReadUnsigned(json, "unlocked_skills_mask", parsed.unlocked_skills_mask) ||
        !ReadUnsigned(json, "unlocked_relics_mask", parsed.unlocked_relics_mask) ||
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
        const auto json = Json::parse(text, nullptr, false);
        if (!json.is_discarded() && FromJson(json, data))
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

template <typename Data>
[[nodiscard]] Result WriteDocument(const std::filesystem::path &path, const Data &data)
{
    try
    {
        auto text = ToJson(data).dump(2);
        text.push_back('\n');
        return WriteText(path, text);
    }
    catch (const std::exception &exception)
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
[[nodiscard]] Result Load(const std::filesystem::path &path, Data &data)
{
    data = Data{};
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
        if (auto restored = WriteDocument(path, backup); !restored)
        {
            return restored;
        }
        data = backup;
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
        return WriteDocument(path, data);
    }
    return Result::Success();
}

template <typename Data>
[[nodiscard]] Result Save(const std::filesystem::path &path, const Data &data)
{
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
    if (status == ReadStatus::Corrupt)
    {
        if (auto preserved = PreserveCorrupt(path); !preserved)
        {
            return preserved;
        }
    }
    return WriteDocument(path, data);
}

} // namespace

SaveStore::SaveStore(std::optional<std::filesystem::path> root_override)
    : root_(root_override ? std::move(*root_override) : DefaultRoot())
{
}

Result SaveStore::LoadSettings(SettingsData &settings) const
{
    return Load(root_ / kSettingsName, settings);
}

Result SaveStore::SaveSettings(const SettingsData &settings) const
{
    if (!ValidSettings(settings))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                               "SettingsData is invalid.");
    }
    return Save(root_ / kSettingsName, settings);
}

Result SaveStore::LoadProfile(ProfileData &profile) const
{
    return Load(root_ / kProfileName, profile);
}

Result SaveStore::SaveProfile(const ProfileData &profile) const
{
    if (!ValidProfile(profile))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                               "ProfileData is invalid.");
    }
    return Save(root_ / kProfileName, profile);
}

} // namespace hs
