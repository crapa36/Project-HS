#pragma once

#include <hs/core/settings.hpp>
#include <hs/core/graphics_settings.hpp>
#include <hs/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>

namespace hs
{

struct ProfileData
{
    // Version 2 removes unused unlock masks; the reader accepts version 1 without rewriting it.
    static constexpr std::uint32_t kSchemaVersion = 2;

    std::uint32_t schema_version{kSchemaVersion};
    std::uint32_t best_level{1};
    std::uint64_t total_wins{};
    std::uint64_t total_kills{};
};

static_assert(std::is_trivially_copyable_v<ProfileData> &&
              std::is_standard_layout_v<ProfileData>);

// A persistence document, not a simulation/render SPSC message. Unknown optional
// schema-2 fields are retained as JSON in opaque_optional_json when loaded.
struct SettingsDocument
{
    static constexpr std::uint32_t kSchemaVersion = 2;

    std::uint32_t schema_version{kSchemaVersion};
    GraphicsSettings graphics{};
    float master_volume{1.0f};
    float bgm_volume{1.0f};
    float sfx_volume{1.0f};
    float ui_volume{1.0f};
    std::array<std::uint16_t, 4> skill_virtual_keys{'Q', 'W', 'E', 'R'};
    std::string opaque_optional_json{"{}"};
    float screen_overlay_intensity{1.0f};
};

class SaveStore
{
  public:
    explicit SaveStore(
        std::optional<std::filesystem::path> root_override = std::nullopt);

    [[nodiscard]] Result LoadSettings(SettingsData &settings) const;
    [[nodiscard]] Result SaveSettings(const SettingsData &settings) const;
    [[nodiscard]] Result LoadSettingsV2(SettingsDocument &settings,
                                        bool *loaded_from_file = nullptr) const;
    [[nodiscard]] Result SaveSettingsV2(const SettingsDocument &settings) const;
    [[nodiscard]] Result LoadProfile(ProfileData &profile) const;
    [[nodiscard]] Result SaveProfile(const ProfileData &profile) const;

  private:
    std::filesystem::path root_;
};

} // namespace hs
