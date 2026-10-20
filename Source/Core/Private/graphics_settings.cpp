#include <hs/core/graphics_settings.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace hs
{
namespace
{

constexpr std::array<GraphicsSettingDescriptor, kGraphicsSettingCount> kDescriptors{{
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) \
    {SettingId::name, #field, GraphicsSettingType::kind, default_token, static_cast<double>(minimum), static_cast<double>(maximum), GraphicsSettingGroup::group, label_key},
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
}};

bool ValidSymbolicToken(std::string_view token) noexcept
{
    if (token.empty())
    {
        return false;
    }
    for (const unsigned char character : token)
    {
        const bool alpha = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '_' && character != '-' && character != '.')
        {
            return false;
        }
    }
    return true;
}

template <typename T>
std::string NumericToken(T value)
{
    std::array<char, 64> text{};
    const auto result = [&] {
        if constexpr (std::is_floating_point_v<T>)
            return std::to_chars(text.data(), text.data() + text.size(), value,
                std::chars_format::general, std::numeric_limits<T>::max_digits10);
        else return std::to_chars(text.data(), text.data() + text.size(), value);
    }();
    return result.ec == std::errc{} ? std::string(text.data(), result.ptr) : std::string{};
}

template <typename T>
bool ParseNumericToken(std::string_view token, T &value)
{
    if (token.empty()) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

template <typename T>
std::string ValueToken(const T &value)
{
    if constexpr (std::is_same_v<T, bool>) return value ? "true" : "false";
    else if constexpr (std::is_arithmetic_v<T>) return NumericToken(value);
    else if constexpr (std::is_enum_v<T>) return std::string(GraphicsEnumToken(value));
    else if constexpr (std::is_same_v<T, GraphicsOutputExtent>)
        return value ? NumericToken(value->width) + "x" + NumericToken(value->height) : "auto";
    else if constexpr (std::is_same_v<T, GraphicsAutoFloat>)
        return value ? NumericToken(*value) : "auto";
    else return value.value;
}

template <typename T>
bool ParseValueToken(std::string_view token, T &value)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        if (token != "true" && token != "false") return false;
        value = token == "true";
    }
    else if constexpr (std::is_arithmetic_v<T>) return ParseNumericToken(token, value);
    else if constexpr (std::is_enum_v<T>)
    {
        const auto parsed = ParseGraphicsEnumToken<T>(token);
        if (!parsed) return false;
        value = *parsed;
    }
    else if constexpr (std::is_same_v<T, GraphicsOutputExtent>)
    {
        if (token == "auto") value.reset();
        else
        {
            const auto separator = token.find('x');
            GraphicsExtent extent;
            if (separator == std::string_view::npos ||
                !ParseNumericToken(token.substr(0, separator), extent.width) ||
                !ParseNumericToken(token.substr(separator + 1), extent.height)) return false;
            value = extent;
        }
    }
    else if constexpr (std::is_same_v<T, GraphicsAutoFloat>)
    {
        if (token == "auto") value.reset();
        else
        {
            float numeric{};
            if (!ParseNumericToken(token, numeric)) return false;
            value = numeric;
        }
    }
    else
    {
        if (!ValidSymbolicToken(token)) return false;
        value.value = token;
    }
    return true;
}

template <typename T>
void ValidateValue(const T &value, SettingId id, double minimum, double maximum,
                   std::vector<GraphicsSettingIssue> &issues)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        (void)value;
        (void)id;
        (void)minimum;
        (void)maximum;
        (void)issues;
    }
    else if constexpr (std::is_same_v<T, std::uint32_t> || std::is_same_v<T, float>)
    {
        if constexpr (std::is_same_v<T, float>)
        {
            if (!std::isfinite(value))
            {
                issues.push_back({id, GraphicsSettingIssueCode::OutOfRange});
                return;
            }
        }
        if (static_cast<double>(value) < minimum || static_cast<double>(value) > maximum)
        {
            issues.push_back({id, GraphicsSettingIssueCode::OutOfRange});
        }
    }
    else if constexpr (std::is_enum_v<T>)
    {
        if (GraphicsEnumToken(value).empty())
        {
            issues.push_back({id, GraphicsSettingIssueCode::InvalidToken});
        }
    }
    else if constexpr (std::is_same_v<T, GraphicsDisplayId> ||
                       std::is_same_v<T, GraphicsProviderId> ||
                       std::is_same_v<T, GraphicsModeId>)
    {
        if (!ValidSymbolicToken(value.value))
        {
            issues.push_back({id, GraphicsSettingIssueCode::InvalidToken});
        }
    }
    else if constexpr (std::is_same_v<T, GraphicsOutputExtent>)
    {
        if (value && (value->width == 0 || value->height == 0))
        {
            issues.push_back({id, GraphicsSettingIssueCode::InvalidExtent});
        }
    }
    else if constexpr (std::is_same_v<T, GraphicsAutoFloat>)
    {
        if (value && (!std::isfinite(*value) || *value < minimum || *value > maximum))
        {
            issues.push_back({id, GraphicsSettingIssueCode::OutOfRange});
        }
    }
    else
    {
        static_assert(sizeof(T) == 0, "Unsupported GraphicsSettings field type");
    }
}

} // namespace

#define HS_DEFINE_GRAPHICS_ENUM_TOKEN(type, ...)                                     \
    std::string_view GraphicsEnumToken(type value) noexcept                          \
    {                                                                                \
        static constexpr auto tokens = std::to_array<std::string_view>({__VA_ARGS__}); \
        const auto index = static_cast<std::size_t>(value);                          \
        return index < tokens.size() ? tokens[index] : std::string_view{};            \
    }

HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsPreset, "low", "medium", "high", "ultra", "custom")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsDisplayMode, "windowed", "borderless_fullscreen")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsHdrMode, "off", "on", "auto")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsResolutionMode, "manual", "provider_quality")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsFrameGenerationMode, "off", "auto", "fixed", "dynamic")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsQuality, "low", "medium", "high", "ultra")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsOptionalQuality, "off", "low", "medium", "high", "ultra")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsShadowMode, "raster")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsAoMode, "off", "auto", "gtao")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsGiMode, "off", "probe", "software_hybrid")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsReflectionMode, "off", "probe", "screen_space")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsAutoToggle, "off", "auto", "on")
HS_DEFINE_GRAPHICS_ENUM_TOKEN(GraphicsMotionBlur, "off", "low", "high")

#undef HS_DEFINE_GRAPHICS_ENUM_TOKEN

std::span<const GraphicsSettingDescriptor> GraphicsSettingDescriptors() noexcept
{
    return kDescriptors;
}

std::string_view GraphicsSettingKey(SettingId id) noexcept
{
    for (const auto &descriptor : kDescriptors)
    {
        if (descriptor.id == id)
        {
            return descriptor.key;
        }
    }
    return {};
}

std::optional<SettingId> ParseGraphicsSettingKey(std::string_view key) noexcept
{
    for (const auto &descriptor : kDescriptors)
    {
        if (descriptor.key == key)
        {
            return descriptor.id;
        }
    }
    return std::nullopt;
}

std::string GraphicsSettingToken(const GraphicsSettings &settings, SettingId id)
{
    switch (id)
    {
#define HS_GRAPHICS_SETTING(number, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) \
    case SettingId::name: return ValueToken(settings.field);
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
    }
    return {};
}

bool SetGraphicsSettingToken(GraphicsSettings &settings, SettingId id, std::string_view token)
{
    auto candidate = settings;
    switch (id)
    {
#define HS_GRAPHICS_SETTING(number, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) \
    case SettingId::name: if (!ParseValueToken(token, candidate.field)) return false; break;
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
    default: return false;
    }
    if (!ValidateGraphicsSettings(candidate).empty()) return false;
    settings = std::move(candidate);
    return true;
}

std::vector<GraphicsSettingIssue> ValidateGraphicsSettings(const GraphicsSettings &settings)
{
    std::vector<GraphicsSettingIssue> issues;
#define HS_GRAPHICS_SETTING(id, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) \
    ValidateValue(settings.field, SettingId::name, static_cast<double>(minimum), static_cast<double>(maximum), issues);
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING

    if (std::isfinite(settings.dynamic_min_percent) &&
        std::isfinite(settings.dynamic_max_percent) &&
        settings.dynamic_min_percent > settings.dynamic_max_percent)
    {
        issues.push_back({SettingId::DynamicMaxPercent, GraphicsSettingIssueCode::InconsistentRange});
    }
    if (settings.hdr_peak_nits && std::isfinite(*settings.hdr_peak_nits) &&
        std::isfinite(settings.hdr_paper_white_nits) &&
        *settings.hdr_peak_nits < settings.hdr_paper_white_nits)
    {
        issues.push_back({SettingId::HdrPeakNits, GraphicsSettingIssueCode::InconsistentRange});
    }
    const auto anisotropy = settings.anisotropic_filtering;
    if (anisotropy >= 1 && anisotropy <= 16 &&
        anisotropy != 1 && anisotropy != 2 && anisotropy != 4 &&
        anisotropy != 8 && anisotropy != 16)
    {
        issues.push_back({SettingId::AnisotropicFiltering, GraphicsSettingIssueCode::OutOfRange});
    }
    return issues;
}

} // namespace hs
