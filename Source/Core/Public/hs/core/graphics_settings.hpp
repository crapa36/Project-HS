#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{

enum class GraphicsPreset : std::uint8_t { Low, Medium, High, Ultra, Custom };
enum class GraphicsDisplayMode : std::uint8_t { Windowed, BorderlessFullscreen };
enum class GraphicsHdrMode : std::uint8_t { Off, On, Auto };
enum class GraphicsResolutionMode : std::uint8_t { Manual, ProviderQuality };
enum class GraphicsFrameGenerationMode : std::uint8_t { Off, Auto, Fixed, Dynamic };
enum class GraphicsQuality : std::uint8_t { Low, Medium, High, Ultra };
enum class GraphicsOptionalQuality : std::uint8_t { Off, Low, Medium, High, Ultra };
enum class GraphicsShadowMode : std::uint8_t { Raster };
enum class GraphicsAoMode : std::uint8_t { Off, Auto, GTAO };
enum class GraphicsGiMode : std::uint8_t { Off, Probe, SoftwareHybrid };
enum class GraphicsReflectionMode : std::uint8_t { Off, Probe, ScreenSpace };
enum class GraphicsAutoToggle : std::uint8_t { Off, Auto, On };
enum class GraphicsMotionBlur : std::uint8_t { Off, Low, High };

// IDs are symbolic tokens. Device/provider availability is resolved outside Core.
struct GraphicsDisplayId { std::string value; bool operator==(const GraphicsDisplayId &) const = default; };
struct GraphicsProviderId { std::string value; bool operator==(const GraphicsProviderId &) const = default; };
struct GraphicsModeId { std::string value; bool operator==(const GraphicsModeId &) const = default; };
struct GraphicsExtent
{
    std::uint32_t width{};
    std::uint32_t height{};
    bool operator==(const GraphicsExtent &) const = default;
};
using GraphicsOutputExtent = std::optional<GraphicsExtent>; // nullopt is Auto.
using GraphicsAutoFloat = std::optional<float>; // nullopt is Auto.

enum class SettingId : std::uint16_t
{
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) name = id,
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
};

inline constexpr std::size_t kGraphicsSettingCount = 0
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) + 1
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
    ;

struct GraphicsSettings
{
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) type field{default_value};
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
};

enum class GraphicsSettingType : std::uint8_t
{
    Boolean, Unsigned, Float, Enum, DisplayId, ProviderId, ModeId, Extent, AutoFloat
};
enum class GraphicsSettingGroup : std::uint8_t
{
    Profile, Display, Reconstruction, FrameGeneration, Assets, Lighting, Effects
};

struct GraphicsSettingDescriptor
{
    SettingId id;
    std::string_view key;
    GraphicsSettingType type;
    std::string_view default_token;
    double minimum; // Applies to numeric kinds; 0 for nonnumeric kinds.
    double maximum;
    GraphicsSettingGroup group;
    std::string_view label_key;
};

enum class GraphicsSettingIssueCode : std::uint8_t
{
    InvalidToken, OutOfRange, InvalidExtent, InconsistentRange
};
struct GraphicsSettingIssue
{
    SettingId id;
    GraphicsSettingIssueCode code;
};

[[nodiscard]] std::span<const GraphicsSettingDescriptor> GraphicsSettingDescriptors() noexcept;
[[nodiscard]] std::string_view GraphicsSettingKey(SettingId id) noexcept;
[[nodiscard]] std::optional<SettingId> ParseGraphicsSettingKey(std::string_view key) noexcept;
[[nodiscard]] std::vector<GraphicsSettingIssue> ValidateGraphicsSettings(
    const GraphicsSettings &settings);
// Definition-backed UI tokens. A rejected edit leaves the complete settings
// object unchanged; device/provider availability is resolved by Runtime.
[[nodiscard]] std::string GraphicsSettingToken(const GraphicsSettings &, SettingId);
[[nodiscard]] bool SetGraphicsSettingToken(GraphicsSettings &, SettingId, std::string_view);

[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsPreset value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsDisplayMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsHdrMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsResolutionMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsFrameGenerationMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsQuality value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsOptionalQuality value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsShadowMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsAoMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsGiMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsReflectionMode value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsAutoToggle value) noexcept;
[[nodiscard]] std::string_view GraphicsEnumToken(GraphicsMotionBlur value) noexcept;

template <typename Enum>
[[nodiscard]] std::optional<Enum> ParseGraphicsEnumToken(std::string_view token) noexcept
{
    if (token.empty())
    {
        return std::nullopt;
    }
    // All Core enum domains are contiguous and have fewer than 16 entries.
    for (std::uint8_t index = 0; index < 16; ++index)
    {
        const auto value = static_cast<Enum>(index);
        if (GraphicsEnumToken(value) == token)
        {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace hs
