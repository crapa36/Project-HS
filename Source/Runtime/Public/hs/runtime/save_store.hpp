#pragma once

#include <hs/core/settings.hpp>
#include <hs/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <type_traits>

namespace hs
{

struct ProfileData
{
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version{kSchemaVersion};
    std::uint32_t best_level{1};
    std::uint64_t total_wins{};
    std::uint64_t total_kills{};
    std::uint64_t unlocked_skills_mask{0xFF};
    std::uint64_t unlocked_relics_mask{0xFFF};
};

static_assert(std::is_trivially_copyable_v<ProfileData> &&
              std::is_standard_layout_v<ProfileData>);

class SaveStore
{
  public:
    explicit SaveStore(
        std::optional<std::filesystem::path> root_override = std::nullopt);

    [[nodiscard]] Result LoadSettings(SettingsData &settings) const;
    [[nodiscard]] Result SaveSettings(const SettingsData &settings) const;
    [[nodiscard]] Result LoadProfile(ProfileData &profile) const;
    [[nodiscard]] Result SaveProfile(const ProfileData &profile) const;

  private:
    std::filesystem::path root_;
};

} // namespace hs
