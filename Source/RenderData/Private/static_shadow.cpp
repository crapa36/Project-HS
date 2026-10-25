#include <hs/render_data/static_shadow.hpp>
#include <hs/core/sha256.hpp>

#include <bit>
#include <cstdint>
#include <span>
#include <string_view>

namespace hs::render_data
{
bool IsStaticShadowCaster(const RenderInstance &source) noexcept
{
    if (source.vfx_owner_id || source.status_visual_mask || source.animation_clip_id.value ||
        HasEnvironmentWind(source.mesh, source.environment_variant)) return false;
    switch (source.mesh)
    {
    case RenderMesh::Ground:
    case RenderMesh::DirtPatch:
    case RenderMesh::TreeTrunk:
    case RenderMesh::TreeCanopy:
    case RenderMesh::Rock:
    case RenderMesh::GroundDetail:
    case RenderMesh::MeadowSurface: return true;
    default: return false;
    }
}

std::array<std::byte, 32> StaticShadowLayoutDigest(const StaticRenderScene &scene)
{
    Sha256Stream hash;
    constexpr std::string_view domain = "HS-STATIC-SHADOW-LAYOUT-1";
    hash.Add(std::as_bytes(std::span(domain.data(), domain.size())));
    const auto word = [&](std::uint32_t value) {
        std::array<std::byte, 4> bytes{};
        for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (i * 8)) & 255);
        hash.Add(bytes);
    };
    const auto scalar = [&](float value) { word(std::bit_cast<std::uint32_t>(value)); };
    const auto identity = [&](std::uint64_t value) { word(std::uint32_t(value)); word(std::uint32_t(value >> 32)); };
    std::uint64_t count{};
    for (const auto &source : scene.instances)
    {
        if (!IsStaticShadowCaster(source)) continue;
        ++count;
        word(static_cast<std::uint32_t>(source.mesh));
        for (float value : {source.position.x, source.position.y, source.position.z, source.yaw,
                            source.scale.x, source.scale.y, source.scale.z}) scalar(value);
        word(source.environment_variant);
        word(source.color_rgba);
        identity(source.asset_id.value);
        identity(source.material_id.value);
        // Canonical terrain is opaque; canopy coverage derives its seed from world XZ.
    }
    identity(count);
    return hash.Finish();
}

} // namespace hs::render_data
