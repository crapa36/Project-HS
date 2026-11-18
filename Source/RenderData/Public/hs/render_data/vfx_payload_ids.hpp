#pragma once
#include <cstdint>
#include <string_view>

namespace hs::vfx_payload
{
// Existing uint32 wire IDs; shared by authoring and prepared consumers.
[[nodiscard]] constexpr std::uint32_t Id(std::string_view name) noexcept
{
    std::uint32_t value = 2166136261u;
    for (const unsigned char c : name)
    {
        value ^= c;
        value *= 16777619u;
    }
    return value;
}

inline constexpr std::uint32_t PointEvent = Id("PointEventPayload");
inline constexpr std::uint32_t PresentationContext = Id("PresentationContextPayload");
inline constexpr std::uint32_t Projectile = Id("ProjectilePayload");
inline constexpr std::uint32_t ProjectilePath = Id("ProjectilePathPayload");
inline constexpr std::uint32_t CircleArea = Id("CircleAreaPayload");
inline constexpr std::uint32_t Cone = Id("ConePayload");
inline constexpr std::uint32_t LineArea = Id("LineAreaPayload");
inline constexpr std::uint32_t RingWithGaps = Id("RingWithGapsPayload");
inline constexpr std::uint32_t SafeGapSector = Id("SafeGapSectorPayload");
inline constexpr std::uint32_t EntityAttachment = Id("EntityAttachmentPayload");
inline constexpr std::uint32_t SourceTarget = Id("SourceTargetPayload");
inline constexpr std::uint32_t GenericGameplayGeometry = Id("GenericGameplayGeometryPayload");
}
