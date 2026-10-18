#include <hs/core/graphics_settings.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

bool HasIssue(const std::vector<hs::GraphicsSettingIssue> &issues, hs::SettingId id,
              hs::GraphicsSettingIssueCode code)
{
    return std::ranges::any_of(issues, [id, code](const auto &issue) {
        return issue.id == id && issue.code == code;
    });
}

} // namespace

int main()
{
    const hs::GraphicsSettings defaults;
    Check(hs::ValidateGraphicsSettings(defaults).empty(), "defaults");
    Check(hs::GraphicsSettingDescriptors().size() == hs::kGraphicsSettingCount, "count");
    Check(hs::kGraphicsSettingCount == 52, "table length");
    for (const auto &descriptor : hs::GraphicsSettingDescriptors())
    {
        Check(!descriptor.key.empty(), "key");
        Check(!descriptor.default_token.empty(), "default token");
        Check(!descriptor.label_key.empty(), "label key");
        Check(hs::GraphicsSettingKey(descriptor.id) == descriptor.key, "key by id");
        Check(hs::ParseGraphicsSettingKey(descriptor.key) == descriptor.id, "id by key");
    }
    Check(!hs::ParseGraphicsSettingKey("missing").has_value(), "unknown key");
    Check(static_cast<unsigned>(hs::SettingId::AdaptiveShading) == 56,
          "retained persistence IDs are not renumbered around removed settings");
    for (const auto key : {"ray_tracing_enabled", "rt_trace_distance_m",
                           "neural_ray_reconstruction", "ray_reconstruction_provider"})
        Check(!hs::ParseGraphicsSettingKey(key), "removed settings have no current descriptor");
    Check(!hs::ParseGraphicsEnumToken<hs::GraphicsPreset>("ultra_rt") &&
              !hs::ParseGraphicsEnumToken<hs::GraphicsAoMode>("rtao") &&
              !hs::ParseGraphicsEnumToken<hs::GraphicsGiMode>("ray_traced") &&
              !hs::ParseGraphicsEnumToken<hs::GraphicsReflectionMode>("hybrid_rt") &&
              !hs::ParseGraphicsEnumToken<hs::GraphicsShadowMode>("hybrid_rt"),
          "legacy modes are read migration only, never current UI/API tokens");
    auto rejected = defaults;
    Check(!hs::SetGraphicsSettingToken(rejected, hs::SettingId::Preset, "ultra_rt"),
          "removed preset cannot be reactivated");
    for (unsigned id = 41; id <= 44; ++id)
        Check(!hs::SetGraphicsSettingToken(rejected, static_cast<hs::SettingId>(id), "true"),
              "removed persisted IDs reject live edits");
    Check(hs::ParseGraphicsEnumToken<hs::GraphicsResolutionMode>("provider_quality") ==
          hs::GraphicsResolutionMode::ProviderQuality, "mode parse");
    Check(!hs::ParseGraphicsEnumToken<hs::GraphicsResolutionMode>("unknown"), "unknown mode");

    hs::GraphicsSettings settings = defaults;
    settings.manual_scale_percent = std::numeric_limits<float>::quiet_NaN();
    settings.reconstruction_provider.value = "invalid token";
    settings.hdr_mode = static_cast<hs::GraphicsHdrMode>(255);
    settings.output_extent = hs::GraphicsExtent{1920, 0};
    settings.anisotropic_filtering = 3;
    auto issues = hs::ValidateGraphicsSettings(settings);
    Check(HasIssue(issues, hs::SettingId::ManualScalePercent,
                   hs::GraphicsSettingIssueCode::OutOfRange), "nonfinite float");
    Check(HasIssue(issues, hs::SettingId::ReconstructionProvider,
                   hs::GraphicsSettingIssueCode::InvalidToken), "provider token");
    Check(HasIssue(issues, hs::SettingId::HdrMode,
                   hs::GraphicsSettingIssueCode::InvalidToken), "enum token");
    Check(HasIssue(issues, hs::SettingId::OutputExtent,
                   hs::GraphicsSettingIssueCode::InvalidExtent), "extent");
    Check(HasIssue(issues, hs::SettingId::AnisotropicFiltering,
                   hs::GraphicsSettingIssueCode::OutOfRange), "anisotropy set");

    settings = defaults;
    settings.dynamic_min_percent = 75;
    settings.dynamic_max_percent = 50;
    settings.hdr_peak_nits = 100.0f;
    issues = hs::ValidateGraphicsSettings(settings);
    Check(HasIssue(issues, hs::SettingId::DynamicMaxPercent,
                   hs::GraphicsSettingIssueCode::InconsistentRange), "dynamic bounds");
    Check(HasIssue(issues, hs::SettingId::HdrPeakNits,
                   hs::GraphicsSettingIssueCode::InconsistentRange), "HDR bounds");

    settings = defaults;
    settings.reconstruction_provider.value = "future_provider.v2";
    settings.reconstruction_quality.value = "future-mode";
    Check(hs::ValidateGraphicsSettings(settings).empty(), "future symbolic tokens");
}
